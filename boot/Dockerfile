# syntax=docker/dockerfile:1
# check=skip=InvalidDefaultArgInFrom
# 9front-OBJTYPE: a 9front tree, rebuilt from this checkout on the
# last one (SEED, boot/images'), served over 9P by a patched u9fs,
# with drawterm.  The context is the checkout; boot/rebuild and
# boot/test run it:
#
#	target out:   the build's products (boot/rebuild's dir)
#	target test:  boot/test of kernel KERNEL and userland TAR, from
#	              the context kernel (default: out's)
#	target image: the image (the default)
#
# The build boots qemu in a RUN: ACCEL=kvm needs --allow
# security.insecure, and a builder that grants it, for /dev/kvm.
# SEED has no default: boot/rebuild and boot/test pass boot/images'
ARG SEED
ARG ALPINE=alpine:3.22@sha256:822614ea121e1d40a33655081af10ef85f0aa2ab9e61f649ceaf0c651822433c
ARG ACCEL=kvm

FROM ${ALPINE} AS u9fs
RUN apk add --no-cache build-base
ADD https://github.com/unofficial-mirror/u9fs.git#d65923fd17e8b158350d3ccd6a4e32b89b15014a /src/u9fs
COPY boot/docker/u9fs/*.patch boot/docker/healthcheck.c /src/
WORKDIR /src/u9fs
RUN for p in ../*.patch; do patch -p1 -F0 <"$p" || exit; done \
 && make CFLAGS="-O2 -I. -D_GNU_SOURCE" && strip u9fs \
 && cc -O2 -Wall -Wextra -Werror -o 9p-healthcheck ../healthcheck.c

# SHA256SUMS names the release's tarballs; ADD refetches it every build.
FROM ${ALPINE} AS drawterm
ARG DRAWTERM=https://github.com/rvs/drawterm/releases/latest/download
ADD --chmod=644 ${DRAWTERM}/SHA256SUMS /src/
WORKDIR /src
RUN for t in $(awk '{print $2}' SHA256SUMS); do wget -q "$DRAWTERM/$t" || exit; done \
 && sha256sum -c SHA256SUMS && mkdir -p /out/bin /out/share/doc/drawterm \
 && for t in *.tar.gz; do d=${t%.tar.gz}; tar xozf "$t" \
	&& mv "$d/drawterm" "/out/bin/drawterm.${d#drawterm-kitty-*-}" || exit; done \
 && mv "$d"/* SHA256SUMS /out/share/doc/drawterm/

# the last tree, with its drawterm and the kernel of the machine that
# builds the next one; and what building and testing need, none of
# which the image ships; and this checkout
FROM ${SEED} AS prep
ARG OBJTYPE=amd64
RUN case $OBJTYPE in arm64) q=qemu-system-aarch64;; riscv64) q=qemu-system-riscv64;; esac \
 && apk add --no-cache bash coreutils tar git qemu-system-x86_64 $q
COPY . /src

FROM prep AS build-kvm
ARG OBJTYPE CONF WHAT=all WORK OUT=/out MEM SMP
RUN --security=insecure /src/boot/rebuild -r /9front -a "$OBJTYPE" -c "$CONF" $WORK "$WHAT" "$OUT"

FROM prep AS build-tcg
ARG OBJTYPE CONF WHAT=all WORK OUT=/out MEM SMP
RUN /src/boot/rebuild -r /9front -a "$OBJTYPE" -c "$CONF" $WORK "$WHAT" "$OUT"

FROM build-${ACCEL} AS build

FROM scratch AS out
COPY --from=build /out/ /

# boot/test's kernel and tar, which boot/test sends as this context
FROM out AS kernel

FROM prep AS test-kvm
ARG OBJTYPE CONF KERNEL TAR WORK MEM SMP
RUN --security=insecure --mount=from=kernel,target=/kernel \
	/src/boot/test -r /9front -a "$OBJTYPE" $WORK "/kernel/${KERNEL:-9$CONF}" ${TAR:+"/kernel/$TAR"}

FROM prep AS test-tcg
ARG OBJTYPE CONF KERNEL TAR WORK MEM SMP
RUN --mount=from=kernel,target=/kernel \
	/src/boot/test -r /9front -a "$OBJTYPE" $WORK "/kernel/${KERNEL:-9$CONF}" ${TAR:+"/kernel/$TAR"}

FROM test-${ACCEL} AS test

FROM ${ALPINE} AS image
ARG OBJTYPE=amd64 REVISION SOURCE
LABEL org.opencontainers.image.title="9front $OBJTYPE" \
      org.opencontainers.image.description="9front's $OBJTYPE userland and kernel, built from $SOURCE $REVISION, served over 9P by u9fs" \
      org.opencontainers.image.revision=$REVISION \
      org.opencontainers.image.source=$SOURCE
# the Linux drawterms are glibc binaries (libc, libm, libz only)
RUN apk add --no-cache python3 socat gcompat zlib
# the Linux names of the tree's Plan 9 owners, which u9fs serves
COPY boot/docker/passwd boot/docker/group /etc/
# --parents: /9front keeps its own owner and mode, not just its contents
COPY --parents --from=build /9front /
# the cpuwait cpu server's nvram, and the next build's machine
COPY boot/docker/cpu.nvram /boot/
COPY --from=build /boot/9pc64cpuwait /boot/
COPY --from=u9fs /src/u9fs/u9fs /src/u9fs/9p-healthcheck /usr/local/bin/
COPY --from=u9fs /src/u9fs/LICENSE /usr/share/licenses/u9fs/
COPY --from=drawterm /out/ /usr/local/
COPY boot/docker/9front /usr/local/bin/
EXPOSE 564
HEALTHCHECK --interval=60s --timeout=5s --start-period=5s CMD ["/usr/local/bin/9p-healthcheck"]
ENTRYPOINT ["/usr/local/bin/9front"]
