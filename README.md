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
`docker-compose.yml`. That builds both images from source (this repo's
`docker/Dockerfile`, InvForge's own repo via a git-context build) — good
for testing local changes to either, slower otherwise.

## docker-compose examples (published images, no repo checkout needed)

For everyone else, `examples/` has ready-to-run compose files using the
published `miklosbagi/sigennut` and `miklosbagi/invforge` images
directly — copy either one down and run it, no `git clone` required.

**Against InvForge** (zero hardware) —
[`examples/docker-compose.emulator.yml`](examples/docker-compose.emulator.yml):

```sh
docker compose -f docker-compose.emulator.yml up -d
upsc sigen@localhost
```

**Against your own SigenStor** —
[`examples/docker-compose.real-device.yml`](examples/docker-compose.real-device.yml):

```sh
SIGENSTOR_HOST=192.168.1.50 docker compose -f docker-compose.real-device.yml up -d
upsc sigen@localhost
```

`SIGENSTOR_HOST` is your device's Modbus TCP `host` or `host:port`
(default port 502 if omitted) — compose refuses to start if it's unset,
rather than silently generating a broken `ups.conf`.

Both examples generate `/etc/nut`'s config entirely inline, via a
one-shot init container writing into a named volume — not compose's own
`configs:` mechanism, which mounts individual files rather than the
directory itself and so doesn't satisfy `startup.sh`'s check that
`/etc/nut` is a real mounted volume (confirmed the hard way). Same
permissions gpdm/nut-upsd's own convention requires either way: `0440`,
owned by uid 100 / gid 101 (see `docker/files/startup.sh`).

**Just InvForge, standalone** (no sigennut/NUT at all — e.g. testing a
different Modbus client against it):

```sh
docker run -d --name invforge \
  -p 5020:502 -p 8080:8080 \
  miklosbagi/invforge:latest \
  --vendor sigenergy --firmware V100R001C21SPC116 \
  --scenario 2026-08-14-idle-full-soc
```

See [InvForge's own README](https://github.com/miklosbagi/invforge) for
its full scenario library and HTTP control API.

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
