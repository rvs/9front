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
mkdir -p "$root"/{bin,dev,env,fd,mnt,net,n,proc,root,srv,shr,tmp,lib/ndb,usr/glenda/tmp,usr/glenda/batch}
cp "$root/adm/timezone/GMT" "$root/adm/timezone/local"
cp "$root/sys/lib/dist/ndb/local" "$root/lib/ndb/local"
printf '%s\n' 'service=batch' 'ip/ipconfig || { echo BATCH-FAIL; exit ipconfig }' >"$root/rc/bin/termrc.local"
printf 'smoke\n' >"$root/usr/glenda/batch/smoke.rc"
ROOT="$root" KERNEL="$root/$arch/$kernel" SMOKE_LOG="$work/console.log" \
expect -f - "$source/boot/qemu" "-$arch" <<'EXPECT'
set timeout 600
log_file $env(SMOKE_LOG)
spawn -noecho {*}$argv
trap {catch {exec kill [exp_pid]}; exit 1} {SIGINT SIGTERM SIGHUP}
set status 1
expect {
	"BATCH-PASS" {set status 0}
	"BATCH-FAIL" {}
	timeout {puts stderr "smoke: timed out"}
	eof {puts stderr "smoke: QEMU exited"}
}
catch {exec kill [exp_pid]}
catch {wait}
exit $status
EXPECT
