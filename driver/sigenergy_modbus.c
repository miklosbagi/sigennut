/*  sigenergy_modbus.c - NUT driver for Sigenergy SigenStor hybrid
 *  inverter/BESS systems, treating the whole plant (inverter + battery)
 *  as a single UPS via its Modbus TCP interface.
 *
 *  Copyright (C)
 *    2026  Miklos Bagi <mb_sigennut@mbag.at>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/*
 * Register addresses and their meanings are sourced from Sigenergy's
 * own official Modbus Protocol spec (V2.9), cross-checked against real
 * device captures. See ../../nut-sigenergy/docs/driver-coding-standards.md
 * and ../../sigennut/docs/register-map.md (sibling research repos) for
 * the full provenance, including one still-open item this driver's
 * OL/OB mapping depends on:
 *
 * R11 (from sigennut's PRD): `on_off_grid_status` (30009) is documented
 * by the spec as the plant's real on/off-grid signal, and that mapping
 * is used directly below -- but it has not yet been validated against
 * an actual real-world grid-loss/grid-return transition on the specific
 * installation this was developed against (no grid was ever wired to
 * that unit). Treat OL/OB as spec-correct-by-design but not yet
 * field-proven; revisit if a real transition is ever observed to behave
 * differently than expected.
 *
 * This is a deliberately minimal first driver (project outline "Phase
 * 1"): OL / OB / LB / CHRG / DISCHRG, battery.charge, input.voltage,
 * input.frequency, ups.realpower. Explicitly NOT yet implemented:
 * battery.voltage (no confirmed pack-level voltage register in the
 * curated set used so far), battery.runtime (Phase 2), per-bit alarm
 * decoding (Phase 3 / R2 -- a coarse ALARM status flag is set instead),
 * and general_alarm7 (30281), which the real device rejects as a solo
 * read's starting address (a confirmed firmware quirk unrelated to
 * whether the register exists) -- reading it correctly requires an
 * anchored range read this driver doesn't need for its current scope.
 *
 * Read-only by design (project non-goal: no charge scheduling, no EPS
 * configuration, no writes of any kind) -- this file contains no
 * modbus_write_* call anywhere.
 */

#include "main.h"
#include <modbus.h>
#include "nut_stdint.h"

#if !(defined NUT_MODBUS_LINKTYPE_STR)
#define NUT_MODBUS_LINKTYPE_STR	"unknown"
#endif

#define DRIVER_NAME	"Sigenergy SigenStor Modbus driver (libmodbus link type: " NUT_MODBUS_LINKTYPE_STR ")"
#define DRIVER_VERSION	"0.01"

/* Plant ("whole installation") register addresses, unit id = plant_addr */
#define REG_ON_OFF_GRID_STATUS		30009	/* U16: 0=on grid, 1=off grid (auto), 2=off grid (manual) */
#define REG_ESS_SOC			30014	/* U16, gain 10: battery state of charge, % */
#define REG_GENERAL_ALARM1		30027	/* U16 bitfield */
#define REG_GENERAL_ALARM2		30028	/* U16 bitfield */
#define REG_GENERAL_ALARM3		30029	/* U16 bitfield */
#define REG_GENERAL_ALARM4		30030	/* U16 bitfield */
#define REG_ESS_POWER			30037	/* S32 (2 regs), gain 1000: <0 discharging, >0 charging, kW */
#define REG_GENERAL_ALARM5		30072	/* U16 bitfield */
#define REG_ESS_DISCHARGE_CUTOFF_SOC	30086	/* U16, gain 10: device's own configured reserve floor, % (0 = unconfigured) */
#define REG_GENERAL_ALARM6		30280	/* U16 bitfield */
#define REG_TOTAL_LOAD_POWER		30284	/* S32 (2 regs), gain 1000: protected-load power, kW */

/* Inverter register addresses, unit id = inverter_addr */
#define REG_MODEL_TYPE			30500	/* STRING, 15 regs */
#define REG_SERIAL_NUMBER		30515	/* STRING, 10 regs */
#define REG_FIRMWARE_VERSION		30525	/* STRING, 15 regs */
#define REG_GRID_FREQUENCY		31002	/* U16, gain 100: live measured grid frequency, Hz */
#define REG_PHASE_A_VOLTAGE		31011	/* U32 (2 regs), gain 100: live measured phase A voltage, V */

#define DEFAULT_PLANT_ADDR		247
#define DEFAULT_INVERTER_ADDR		1
#define DEFAULT_LOW_BATTERY_SOC		20.0	/* used when the device's own cutoff (30086) reads 0/unconfigured */

/* driver description structure */
upsdrv_info_t upsdrv_info = {
	DRIVER_NAME,
	DRIVER_VERSION,
	"Miklos Bagi <mb_sigennut@mbag.at>",
	DRV_EXPERIMENTAL,
	{ NULL }
};

static modbus_t *mbctx = NULL;
static int plant_addr = DEFAULT_PLANT_ADDR;
static int inverter_addr = DEFAULT_INVERTER_ADDR;
static double low_battery_soc = DEFAULT_LOW_BATTERY_SOC;
static int errcount = 0;

/* Parse "host[:port]" (default_port used when no ':port' suffix is
 * given) into separately bounded host/port buffers. Adapted from
 * apc_modbus.c's _apc_modbus_parse_host_port(), which sizes its
 * buffers from the actual input length rather than sizeof(a pointer)
 * -- unlike generic_modbus.c's modbus_new() helper, which allocates
 * only sizeof(char*) bytes for its port buffer regardless of the
 * input's real length (works by accident for typical short port
 * numbers, but is the wrong size for the general case, so not reused
 * here). No IPv6 bracket handling needed: Sigenergy's Modbus TCP
 * interface is IPv4/hostname only.
 */
static int parse_host_port(const char *input, char *host, size_t host_buf_size,
	char *port, size_t port_buf_size, uint16_t default_port)
{
	const char *colon;
	size_t host_size, port_size;
	int port_int;

	colon = strchr(input, ':');
	if (!colon) {
		if (snprintf(host, host_buf_size, "%s", input) >= (int) host_buf_size) {
			upslogx(LOG_ERR, "parse_host_port: host too long");
			return 0;
		}
		if (snprintf(port, port_buf_size, "%u", default_port) >= (int) port_buf_size) {
			upslogx(LOG_ERR, "parse_host_port: port buffer too small");
			return 0;
		}
		return 1;
	}

	host_size = (size_t) (colon - input) + 1;	/* +1 for NUL */
	port_size = strlen(colon + 1) + 1;
	if (host_size > host_buf_size || port_size > port_buf_size) {
		upslogx(LOG_ERR, "parse_host_port: buffer too small for '%s'", input);
		return 0;
	}

	snprintf(host, host_size, "%s", input);
	snprintf(port, port_size, "%s", colon + 1);

	port_int = atoi(port);
	if (port_int <= 0 || port_int > 65535) {
		upslogx(LOG_ERR, "parse_host_port: port out of range in '%s'", input);
		return 0;
	}

	return 1;
}

/* Read `count` holding registers (Modbus FC3 -- confirmed, repeatedly,
 * to be what this device actually responds to; its own spec labels the
 * same tables "input register" but its own worked examples use FC3,
 * and that matches every real capture this driver's register choices
 * are based on) from `unit`, into `dest`. Tracks failures in errcount
 * so upsdrv_updateinfo() can decide whether to trust this poll cycle at
 * all, rather than silently publishing a partial read as if it were
 * complete.
 *
 * Returns 0 on success, -1 on failure -- deliberately normalized rather
 * than passing through modbus_read_registers()'s own return value
 * (the number of registers read, e.g. 1 or 2, never 0, on success),
 * so every call site can use a single, uniform success check. */
static int read_regs(int unit, int addr, int count, uint16_t *dest)
{
	if (modbus_set_slave(mbctx, unit) < 0) {
		upslogx(LOG_ERR, "read_regs: invalid unit id %d", unit);
		errcount++;
		return -1;
	}

	if (modbus_read_registers(mbctx, addr, count, dest) == -1) {
		upslogx(LOG_ERR, "read_regs: unit %d addr %d count %d: %s",
			unit, addr, count, modbus_strerror(errno));
		errcount++;
		return -1;
	}
	return 0;
}

static uint32_t combine_u32(const uint16_t *regs)
{
	return ((uint32_t) regs[0] << 16) | regs[1];
}

/* Two's-complement reinterpretation of a combined 32-bit Modbus value.
 * Implementation-defined by the C standard for values above INT32_MAX,
 * but universally two's-complement in practice on every platform NUT
 * targets -- the same assumption InvForge's own decode helpers make,
 * cross-checked against real device captures throughout this project's
 * research (see sigennut/docs/register-map.md). */
static int32_t combine_s32(const uint16_t *regs)
{
	return (int32_t) combine_u32(regs);
}

/* Read a STRING register (big-endian byte pairs, NUL-padded) into a
 * bounded output buffer. `buf_size` must be at least count*2 + 1. */
static int read_string(int unit, int addr, int count, char *buf, size_t buf_size)
{
	uint16_t regs[32];
	int i;
	size_t out_len;

	if ((size_t) count > sizeof(regs) / sizeof(regs[0])) {
		upslogx(LOG_ERR, "read_string: count %d exceeds internal buffer", count);
		return -1;
	}
	if (buf_size < (size_t) count * 2 + 1) {
		upslogx(LOG_ERR, "read_string: output buffer too small for %d registers", count);
		return -1;
	}

	if (read_regs(unit, addr, count, regs) == -1) {
		return -1;
	}

	for (i = 0; i < count; i++) {
		buf[i * 2] = (char) ((regs[i] >> 8) & 0xFF);
		buf[i * 2 + 1] = (char) (regs[i] & 0xFF);
	}
	buf[count * 2] = '\0';

	/* Trim trailing NULs the device pads short strings with */
	out_len = strlen(buf);
	while (out_len > 0 && buf[out_len - 1] == '\0') {
		out_len--;
	}
	buf[out_len] = '\0';

	return 0;
}

void upsdrv_initinfo(void)
{
	char model[32], serial[24], firmware[32];
	uint16_t reg;

	upsdebugx(2, "upsdrv_initinfo");

	dstate_setinfo("device.mfr", "Sigenergy");
	dstate_setinfo("ups.mfr", "Sigenergy");

	if (read_string(inverter_addr, REG_MODEL_TYPE, 15, model, sizeof(model)) == 0) {
		dstate_setinfo("device.model", "%s", model);
		dstate_setinfo("ups.model", "%s", model);
	}
	if (read_string(inverter_addr, REG_SERIAL_NUMBER, 10, serial, sizeof(serial)) == 0) {
		dstate_setinfo("device.serial", "%s", serial);
		dstate_setinfo("ups.serial", "%s", serial);
	}
	if (read_string(inverter_addr, REG_FIRMWARE_VERSION, 15, firmware, sizeof(firmware)) == 0) {
		dstate_setinfo("ups.firmware", "%s", firmware);
	}

	/* Low-battery threshold: prefer an explicit operator override,
	 * then the device's own configured reserve floor -- but that
	 * register reads 0 ("unconfigured") on every real installation
	 * seen so far, which would mean LB never triggers, so fall back
	 * to a conservative hardcoded default rather than trust a 0 as
	 * "the operator wants zero reserve". See the project outline's
	 * "Low-Battery Policy" section: BESS reserve is a policy choice,
	 * not something to blindly inherit from the device. */
	if (getval("low_battery_soc")) {
		low_battery_soc = strtod(getval("low_battery_soc"), NULL);
	} else if (read_regs(plant_addr, REG_ESS_DISCHARGE_CUTOFF_SOC, 1, &reg) == 0 && reg > 0) {
		low_battery_soc = reg / 10.0;
	}
	dstate_setinfo("battery.charge.low", "%.1f", low_battery_soc);

	upsh.instcmd = NULL;
}

void upsdrv_updateinfo(void)
{
	uint16_t grid_status, soc, alarm1, alarm2, alarm3, alarm4, alarm5, alarm6, freq;
	uint16_t ess_power_regs[2], load_power_regs[2], voltage_regs[2];
	double soc_pct, ess_power_kw, load_power_kw;

	errcount = 0;

	if (read_regs(plant_addr, REG_ON_OFF_GRID_STATUS, 1, &grid_status) == -1
		|| read_regs(plant_addr, REG_ESS_SOC, 1, &soc) == -1
		|| read_regs(plant_addr, REG_ESS_POWER, 2, ess_power_regs) == -1) {
		dstate_datastale();
		return;
	}

	soc_pct = soc / 10.0;
	ess_power_kw = combine_s32(ess_power_regs) / 1000.0;

	dstate_setinfo("battery.charge", "%.1f", soc_pct);

	if (read_regs(plant_addr, REG_TOTAL_LOAD_POWER, 2, load_power_regs) == 0) {
		load_power_kw = combine_s32(load_power_regs) / 1000.0;
		dstate_setinfo("ups.realpower", "%.0f", load_power_kw * 1000.0);
	}

	if (read_regs(inverter_addr, REG_GRID_FREQUENCY, 1, &freq) == 0) {
		dstate_setinfo("input.frequency", "%.2f", freq / 100.0);
	}
	if (read_regs(inverter_addr, REG_PHASE_A_VOLTAGE, 2, voltage_regs) == 0) {
		dstate_setinfo("input.voltage", "%.1f", combine_u32(voltage_regs) / 100.0);
	}

	status_init();

	if (grid_status == 0) {
		status_set("OL");
	} else {
		status_set("OB");
	}

	if (ess_power_kw > 0.01) {
		status_set("CHRG");
	} else if (ess_power_kw < -0.01) {
		status_set("DISCHRG");
	}

	if (soc_pct <= low_battery_soc) {
		status_set("LB");
	}

	/* Coarse alarm presence only -- per-bit decoding against the
	 * spec's Appendix 2-13 enumerations is deferred, see this file's
	 * header comment (R2/Phase 3). general_alarm7 (30281) is
	 * deliberately excluded here, see header comment. */
	if (read_regs(plant_addr, REG_GENERAL_ALARM1, 1, &alarm1) == 0
		&& read_regs(plant_addr, REG_GENERAL_ALARM2, 1, &alarm2) == 0
		&& read_regs(plant_addr, REG_GENERAL_ALARM3, 1, &alarm3) == 0
		&& read_regs(plant_addr, REG_GENERAL_ALARM4, 1, &alarm4) == 0
		&& read_regs(plant_addr, REG_GENERAL_ALARM5, 1, &alarm5) == 0
		&& read_regs(plant_addr, REG_GENERAL_ALARM6, 1, &alarm6) == 0) {
		if (alarm1 || alarm2 || alarm3 || alarm4 || alarm5 || alarm6) {
			status_set("ALARM");
		}
	}

	status_commit();

	if (errcount == 0) {
		dstate_dataok();
	} else {
		dstate_datastale();
	}
}

void upsdrv_shutdown(void)
{
	/* Read-only by design for this first driver revision (project
	 * non-goal: no charge scheduling, no EPS control, no writes of
	 * any kind) -- there is no Sigenergy register this driver is
	 * prepared to write to trigger a shutdown. upsmon's shutdown
	 * sequence for this UPS should rely on the *server* the driver is
	 * protecting shutting itself down on LB/FSD, not on this driver
	 * commanding the SigenStor to do anything. */
	upslogx(LOG_ERR, "shutdown not supported (read-only driver)");
	if (handling_upsdrv_shutdown > 0) {
		set_exit_flag(EF_EXIT_FAILURE);
	}
}

void upsdrv_help(void)
{
	printf("\nSigenergy-specific options:\n"
		"  plant_addr: Modbus unit id for plant-level registers (default: %d)\n"
		"  inverter_addr: Modbus unit id for inverter-level registers (default: %d)\n"
		"  low_battery_soc: LB threshold in %% state-of-charge (default: device's own\n"
		"    configured reserve floor if nonzero, else %.0f)\n",
		DEFAULT_PLANT_ADDR, DEFAULT_INVERTER_ADDR, DEFAULT_LOW_BATTERY_SOC);
}

void upsdrv_tweak_prognames(void)
{
}

void upsdrv_makevartable(void)
{
	addvar(VAR_VALUE, "plant_addr", "Modbus unit id for plant-level registers");
	addvar(VAR_VALUE, "inverter_addr", "Modbus unit id for inverter-level registers");
	addvar(VAR_VALUE, "low_battery_soc", "Low-battery threshold, %% state-of-charge");
}

void upsdrv_initups(void)
{
	char host[256], port_str[8];
	uint16_t port;

	upsdebugx(2, "upsdrv_initups");

	if (getval("plant_addr")) {
		plant_addr = atoi(getval("plant_addr"));
	}
	if (getval("inverter_addr")) {
		inverter_addr = atoi(getval("inverter_addr"));
	}

	if (!parse_host_port(device_path, host, sizeof(host), port_str, sizeof(port_str), 502)) {
		fatalx(EXIT_FAILURE, "upsdrv_initups: invalid port value '%s' (expected host[:port])", device_path);
	}
	port = (uint16_t) atoi(port_str);

	/* modbus_new_tcp() takes ctx_tcp->ip as a raw 16-byte buffer and
	 * modbus_connect() feeds it straight to inet_pton() -- it never
	 * resolves hostnames, only dotted-quad IPv4 literals (confirmed
	 * against libmodbus 3.1.10 source; a hostname just fails to
	 * connect with no useful errno). modbus_new_tcp_pi() ("protocol
	 * independent") resolves via getaddrinfo() instead, so it accepts
	 * a real hostname or an IP, matching how apc_modbus.c -- the
	 * driver parse_host_port() above was adapted from -- constructs
	 * its own TCP context. */
	mbctx = modbus_new_tcp_pi(host, port_str);
	if (mbctx == NULL) {
		fatalx(EXIT_FAILURE, "upsdrv_initups: modbus_new_tcp_pi: unable to create context for %s:%u", host, port);
	}

	if (modbus_connect(mbctx) == -1) {
		modbus_free(mbctx);
		mbctx = NULL;
		fatalx(EXIT_FAILURE, "upsdrv_initups: modbus_connect to %s:%u failed: %s", host, port, modbus_strerror(errno));
	}

	upslogx(LOG_INFO, "connected to %s:%u (plant unit %d, inverter unit %d)", host, port, plant_addr, inverter_addr);
}

void upsdrv_cleanup(void)
{
	if (mbctx != NULL) {
		modbus_close(mbctx);
		modbus_free(mbctx);
		mbctx = NULL;
	}
}
