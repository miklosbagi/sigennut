# sigennut -- NUT + sigenergy_modbus, built from source
#
# Lets people test the Sigenergy SigenStor NUT driver today, without
# waiting on it to land upstream in networkupstools/nut. Same
# operational shape as gpdm/nut-upsd
# (https://github.com/gpdm/nut/tree/master/nut-upsd): Alpine-based,
# config supplied entirely via a volume-mounted /etc/nut (no env-var
# config), started by files/startup.sh (copied near-verbatim, BSD
# license notice intact) which gates on the config volume/permissions
# then runs `upsdrvctl start` followed by `exec upsd -D`.
#
# Unlike gpdm's image (which apk-installs stock NUT only), this one
# clones upstream NUT fresh at build time, vendors in
# ../driver/sigenergy_modbus.c (this driver isn't in NUT yet), and
# applies ../driver/sigenergy_modbus-makefile.patch to wire it into the
# build -- then, same as gpdm, apk-installs stock NUT for upsd/
# upsdrvctl/the nut user, and splices in the freshly compiled driver
# binary on top of it.

ARG NUT_REF=master

FROM alpine:edge AS builder

ARG NUT_REF

RUN apk add --no-cache \
	build-base \
	autoconf \
	automake \
	libtool \
	pkgconf \
	libmodbus-dev \
	git

WORKDIR /src

RUN git clone --depth 1 --branch "${NUT_REF}" https://github.com/networkupstools/nut.git

COPY driver/sigenergy_modbus.c nut/drivers/sigenergy_modbus.c
COPY driver/sigenergy_modbus-makefile.patch /tmp/sigenergy_modbus-makefile.patch

WORKDIR /src/nut

# Fails loudly (non-zero exit, build stops) if upstream's drivers/Makefile.am
# has drifted enough that this patch no longer applies cleanly -- better
# than silently producing an image without the driver wired in.
RUN git apply /tmp/sigenergy_modbus-makefile.patch

# Same configure flags as Alpine's own 'nut' aport (confirmed via
# `strings` against its shipped driver binaries), so our compiled
# driver's CONFPATH/STATEPATH agree with the apk-installed upsd/
# upsdrvctl it'll run alongside in the runtime stage below.
# WITHOUT_NUT_AUGEAS: autogen.sh wants Python to regenerate the Augeas
# lens for ups.conf parsing; skip it rather than add a whole Python
# toolchain to the builder stage for a feature this image doesn't use.
ENV WITHOUT_NUT_AUGEAS=true

RUN ./autogen.sh && \
	./configure --with-modbus --with-drivers=sigenergy_modbus \
		--with-serial=no --with-usb=no --with-snmp=no --with-macosx-ups=no \
		--prefix=/usr --sysconfdir=/etc/nut --with-statepath=/var/run/nut \
		--with-user=nut --with-group=nut && \
	make -C drivers sigenergy_modbus


FROM alpine:edge

LABEL maintainer="Miklos Bagi <mb_sigennut@mbag.at>"

ARG BUILD_DATE
ARG VCS_REF
LABEL org.label-schema.build-date=$BUILD_DATE \
	org.label-schema.schema-version="1.0" \
	org.label-schema.description="NUT upsd + sigenergy_modbus driver, built from source" \
	org.label-schema.name=sigennut \
	org.label-schema.vcs-ref=$VCS_REF \
	org.label-schema.vcs-url=https://github.com/miklosbagi/sigennut

# 'nut' package is only available from the testing branch, same as gpdm/nut-upsd.
RUN echo "http://dl-cdn.alpinelinux.org/alpine/edge/testing" >> /etc/apk/repositories

RUN apk add --no-cache nut libmodbus

# rename included sample config files out of the way, same as gpdm/nut-upsd
RUN [ -d /etc/nut ] && find /etc/nut/ -type f -exec mv {} {}.sample \; || false

# splice in our driver -- the one thing Alpine's stock 'nut' package doesn't have yet
COPY --from=builder /src/nut/drivers/sigenergy_modbus /usr/lib/nut/sigenergy_modbus
RUN chmod 755 /usr/lib/nut/sigenergy_modbus

COPY docker/files/startup.sh /startup.sh
RUN chmod 700 /startup.sh

RUN mkdir -p /var/run/nut && \
	chown nut:nut /var/run/nut && \
	chmod 700 /var/run/nut

ENTRYPOINT ["/startup.sh"]

EXPOSE 3493
