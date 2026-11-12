#!/usr/bin/env bash
# Usage: smoke.sh ARCH KERNEL ARCH.tar.gz
set -euo pipefail
arch=$1; kernel=$2
source=$(realpath "$(dirname "$0")/../..")
work=$(realpath -m "${SMOKE_WORK:-/tmp/9front-smoke}")
root=$work/root
mkdir -p "$work"
mkdir "$root"
git -C "$source" archive HEAD | tar -xf - -C "$root"
tar -xzf "$3" -C "$root"
mkdir -p "$root"/{bin,dev,env,fd,mnt,net,n,proc,root,srv,shr,tmp,usr/glenda/tmp}
cp "$root/adm/timezone/GMT" "$root/adm/timezone/local"
cp "$source/.github/ci/smoke.c" "$root/.smoke.c"
: >"$root/rc/bin/termrc"
cat >"$root/usr/glenda/lib/profile" <<'RC'
rc -e /.smoke.rc
if(~ $status '') echo SMOKE-OK
if not echo SMOKE-FAIL
while() sleep 60
RC
cat >"$root/.smoke.rc" <<'RC'
objtype=$cputype
path=(/$cputype/bin /rc/bin .)
NPROC=1
cd /tmp
cp /.smoke.c smoke.c
echo '</$objtype/mkfile
TARG=smoke
OFILES=smoke.$O
</sys/src/cmd/mkone' >mkfile
mk install 'BIN=/tmp'
./smoke
ip/ipconfig -g 10.0.2.2 ether /net/ether0 10.0.2.15 255.255.255.0
ip/ping -n 3 10.0.2.2
echo 'smoke: network ok'
RC
ROOT="$root" KERNEL="$root/$arch/$kernel" SMOKE_LOG="$work/console.log" \
expect -f - "$source/boot/qemu" "-$arch" <<'EXPECT'
set timeout 600
log_file $env(SMOKE_LOG)
spawn -noecho {*}$argv
trap {catch {exec kill [exp_pid]}; exit 1} {SIGINT SIGTERM SIGHUP}
set status 1
expect {
	"SMOKE-OK" {set status 0}
	"SMOKE-FAIL" {}
	timeout {puts stderr "smoke: timed out"}
	eof {puts stderr "smoke: QEMU exited"}
}
catch {exec kill [exp_pid]}
catch {wait}
exit $status
EXPECT
