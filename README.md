🇬🇧 [English](README.md) | 🇭🇺 [Magyar](README.hu.md)

# sigennut

A [Network UPS Tools (NUT)](https://networkupstools.org/) driver that lets
a Sigenergy SigenStor hybrid inverter/battery system be monitored as a UPS
over its Modbus TCP interface — `battery.charge`, `ups.status`
(`OL`/`OB`/`LB`/`CHRG`/`DISCHRG`), `input.voltage`, `input.frequency`,
`ups.realpower`, and device identity, straight from the plant and inverter
Modbus registers.

**Status: not upstream yet.** [`driver/sigenergy_modbus.c`](driver/sigenergy_modbus.c)
is written against NUT's own driver API and coding standards, aimed at
eventual inclusion in [`networkupstools/nut`](https://github.com/networkupstools/nut)
directly — but that PR hasn't been opened yet. This repo exists so you can
test the driver *now*, against your own SigenStor, without waiting on that.
This image builds NUT from source (upstream `master`, by default) and
vendors this driver into it via [`driver/sigenergy_modbus-makefile.patch`](driver/sigenergy_modbus-makefile.patch)
— once the driver is merged upstream, this repo becomes unnecessary for
anyone running a NUT version recent enough to include it.

Read-only by design: this driver contains no `modbus_write_*` call
anywhere. It won't reconfigure your inverter, schedule charging, or touch
EPS settings.

One known open item: `on_off_grid_status` (register 30009) is used as the
`OL`/`OB` signal. It's documented by Sigenergy's own Modbus spec as the
correct signal for exactly this purpose, but hasn't yet been field-verified
against a real grid-loss/grid-return transition. Treat `OL`/`OB` as
spec-correct-by-design but not yet field-proven.

## Quickstart (no hardware required)

```sh
docker compose up -d --build
upsc sigen@localhost
```

This builds and runs `sigennut` alongside
[InvForge](https://github.com/miklosbagi/invforge), a multi-vendor
inverter/BESS simulator, pre-loaded with real captured SigenStor register
data — so you can see it working with zero hardware. See
`docker-compose.yml`.

## Running against your own SigenStor

```sh
mkdir -p config
cp docker/files/ups.conf.sample config/ups.conf
cp docker/files/upsd.conf.sample config/upsd.conf
cp docker/files/upsd.users.sample config/upsd.users
# edit config/ups.conf: set `port` to your device's host:port (Modbus TCP, default 502)
```

Then build and run just the `sigennut` image (skip the `invforge` service),
mounting `./config` at `/etc/nut` instead of the `nut-config` volume the
compose demo uses — the same `nut-config-init` approach in
`docker-compose.yml` works here too if you'd rather not deal with
permissions by hand; gpdm/nut-upsd's own convention (which this image
follows) requires `/etc/nut`'s files to be exactly mode `0440`, owned by
uid 100 / gid 101 (see `docker/files/startup.sh`).

```sh
docker build -f docker/Dockerfile -t sigennut .
docker run -d --name sigennut \
  -v "$(pwd)/config:/etc/nut:ro" \
  -p 3493:3493 \
  sigennut
upsc sigen@localhost
```

## How the image is built

`docker/Dockerfile` is a two-stage build:

1. **builder** — shallow-clones upstream NUT (`ARG NUT_REF`, default
   `master`), copies in `driver/sigenergy_modbus.c`, applies
   `driver/sigenergy_modbus-makefile.patch` (fails the build loudly if
   upstream's `drivers/Makefile.am` has drifted too far for the patch to
   apply — better than silently shipping an image without the driver
   wired in), then configures and compiles just this one driver with the
   same `--sysconfdir=/etc/nut --with-statepath=/var/run/nut
   --with-user=nut --with-group=nut` flags Alpine's own `nut` package
   uses, so the compiled binary's paths agree with the rest of the image.
2. **runtime** — apk-installs Alpine's stock `nut` package (this gives
   `upsd`, `upsdrvctl`, the `nut` user/group, and every other stock driver
   for free — no need to reinvent that packaging), then splices the
   freshly compiled `sigenergy_modbus` binary in over it.

Same operational conventions as
[`gpdm/nut-upsd`](https://github.com/gpdm/nut/tree/master/nut-upsd), which
this is modeled on: config is 100% file-based via a required `/etc/nut`
volume mount, `docker/files/startup.sh` (copied near-verbatim, BSD license
notice intact) refuses to start unless that volume is mounted and every
config file is exactly mode `0440` owned by `nut:nut`, then runs
`upsdrvctl start` followed by `exec upsd -D`.

## License

GPL-2.0-or-later — see [LICENSE](LICENSE). Same as
[NUT](https://github.com/networkupstools/nut) itself and
[`driver/sigenergy_modbus.c`](driver/sigenergy_modbus.c)'s own header,
since this image redistributes both.

`docker/files/startup.sh` is copied from
[`gpdm/nut-upsd`](https://github.com/gpdm/nut/tree/master/nut-upsd) under
its own BSD Simplified License (notice retained in the file).
