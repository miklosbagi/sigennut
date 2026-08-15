🇬🇧 [English](README.md) | 🇭🇺 [Magyar](README.hu.md)

# sigennut

Egy [Network UPS Tools (NUT)](https://networkupstools.org/) driver, amely
lehetővé teszi, hogy egy Sigenergy SigenStor hibrid inverter/akkumulátoros
rendszert UPS-ként lehessen felügyelni a Modbus TCP interfészén keresztül
— `battery.charge`, `ups.status` (`OL`/`OB`/`LB`/`CHRG`/`DISCHRG`),
`input.voltage`, `input.frequency`, `ups.realpower`, valamint eszköz-
azonosítás, közvetlenül a plant és inverter Modbus regisztereiből.

**Állapot: még nincs upstreamben.** A
[`driver/sigenergy_modbus.c`](driver/sigenergy_modbus.c) a NUT saját
driver API-ja és kódolási szabályai szerint íródott, azzal a céllal, hogy
végül közvetlenül bekerüljön a
[`networkupstools/nut`](https://github.com/networkupstools/nut) projektbe
— de az ehhez tartozó PR még nincs megnyitva. Ez a repó azért létezik,
hogy a drivert *már most* tesztelhesd, a saját SigenStorodon, anélkül
hogy erre várnod kellene. Ez az image forrásból építi a NUT-ot (alapból
az upstream `master` ágból), és beépíti ezt a drivert a
[`driver/sigenergy_modbus-makefile.patch`](driver/sigenergy_modbus-makefile.patch)
segítségével — amint a driver bekerül az upstreambe, ez a repó feleslegessé
válik mindenki számára, aki elég friss NUT-verziót futtat ahhoz, hogy az
már tartalmazza.

Tervezésénél fogva csak olvas: ez a driver sehol nem tartalmaz
`modbus_write_*` hívást. Nem konfigurálja át az invertert, nem ütemez
töltést, és nem nyúl az EPS-beállításokhoz.

Egy ismert nyitott kérdés: az `on_off_grid_status` (30009-es regiszter)
szolgál `OL`/`OB` jelzésként. A Sigenergy saját Modbus specifikációja
pontosan erre a célra dokumentálja mint a helyes jelzést, de ezt még nem
igazoltuk terepen egy valódi hálózat-kiesés/hálózat-visszatérés
átmeneten. Az `OL`/`OB`-t kezeld úgy, mint ami a specifikáció szerint
helyes, de terepen még nem bizonyított.

## Gyorsindítás (hardver nélkül)

```sh
docker compose up -d --build
upsc sigen@localhost
```

Ez felépíti és elindítja a `sigennut`-ot az
[InvForge](https://github.com/miklosbagi/invforge) mellett, amely egy
több gyártót támogató inverter/BESS szimulátor, valódi, rögzített
SigenStor regiszteradatokkal feltöltve — így hardver nélkül is láthatod
működés közben. Lásd a `docker-compose.yml`-t.

## Futtatás a saját SigenStorod ellen

```sh
mkdir -p config
cp docker/files/ups.conf.sample config/ups.conf
cp docker/files/upsd.conf.sample config/upsd.conf
cp docker/files/upsd.users.sample config/upsd.users
# szerkeszd a config/ups.conf-ot: állítsd a `port`-ot a saját eszközöd host:port címére (Modbus TCP, alapból 502)
```

Ezután építsd fel és futtasd csak a `sigennut` image-et (az `invforge`
service-t kihagyva), a `./config`-ot a `nut-config` kötet helyett az
`/etc/nut`-ra mountolva — a `docker-compose.yml`-ben lévő
`nut-config-init` megközelítés itt is működik, ha nem szeretnél kézzel
bajlódni a jogosultságokkal; a gpdm/nut-upsd saját konvenciója (amelyet
ez az image követ) megköveteli, hogy az `/etc/nut` fájljai pontosan
`0440` móddal, uid 100 / gid 101 tulajdonossal rendelkezzenek (lásd
`docker/files/startup.sh`).

```sh
docker build -f docker/Dockerfile -t sigennut .
docker run -d --name sigennut \
  -v "$(pwd)/config:/etc/nut:ro" \
  -p 3493:3493 \
  sigennut
upsc sigen@localhost
```

## Hogyan épül fel az image

A `docker/Dockerfile` egy kétlépcsős build:

1. **builder** — sekély (shallow) klónozást végez az upstream NUT-ból
   (`ARG NUT_REF`, alapból `master`), bemásolja a
   `driver/sigenergy_modbus.c`-t, alkalmazza a
   `driver/sigenergy_modbus-makefile.patch`-et (hangosan elbukik a build,
   ha az upstream `drivers/Makefile.am`-ja túl sokat változott ahhoz,
   hogy a patch alkalmazható legyen — ez jobb, mintha csendben olyan
   image készülne, amelybe a driver nincs bekötve), majd konfigurálja és
   lefordítja csak ezt az egy drivert, ugyanazokkal a
   `--sysconfdir=/etc/nut --with-statepath=/var/run/nut --with-user=nut
   --with-group=nut` kapcsolókkal, amelyeket az Alpine saját `nut`
   csomagja is használ, hogy a lefordított bináris útvonalai
   megegyezzenek az image többi részével.
2. **runtime** — apk-val telepíti az Alpine gyári `nut` csomagját (ez
   ingyen adja az `upsd`-t, az `upsdrvctl`-t, a `nut` felhasználót/
   csoportot, és minden más gyári drivert — nem kell újra feltalálni azt
   a csomagolást), majd beilleszti a frissen lefordított
   `sigenergy_modbus` binárist a helyére.

Ugyanazok a működési konvenciók, mint a
[`gpdm/nut-upsd`](https://github.com/gpdm/nut/tree/master/nut-upsd)
esetében, amelyre ez épül: a konfiguráció 100%-ban fájl-alapú, egy
kötelező `/etc/nut` kötet-mounton keresztül, a `docker/files/startup.sh`
(szinte szó szerint átvéve, a BSD licenc-megjegyzéssel együtt) nem indul
el, hacsak ez a kötet nincs mountolva, és minden konfigurációs fájl
pontosan `0440` móddal, `nut:nut` tulajdonossal rendelkezik, majd lefut az
`upsdrvctl start`, azt követően pedig az `exec upsd -D`.

## Licenc

GPL-2.0-or-later — lásd [LICENSE](LICENSE). Ugyanaz, mint maga a
[NUT](https://github.com/networkupstools/nut), és a
[`driver/sigenergy_modbus.c`](driver/sigenergy_modbus.c) saját fejléce,
mivel ez az image mindkettőt újraterjeszti.

A `docker/files/startup.sh` a
[`gpdm/nut-upsd`](https://github.com/gpdm/nut/tree/master/nut-upsd)
projektből lett átvéve, annak saját BSD Simplified licence alatt (a
megjegyzés a fájlban megmaradt).
