#!/usr/bin/env bash
# Usage: KCI_SEED=amd64.tar.gz .github/ci/build.sh ARCH
# Runs in a fresh, disposable checkout and writes .ci/ARCH.tar.gz.
set -euo pipefail

(( $# == 1 )) || { echo 'usage: build.sh ARCH' >&2; exit 1; }
arch=$1
cd "$(dirname "$0")/../.."
seed=${KCI_SEED:-amd64.tar.gz}

mkdir .ci
tar -xf "$seed"
mv amd64/9pc64cpuwait .ci/kernel
chmod -R u+rwX amd64/bin amd64/lib amd64/init
mkdir -p {bin,dev,env,fd,mnt,net,n,proc,root,srv,shr,tmp}
mkdir -p rc/bin/postscript usr/glenda/tmp usr/glenda/batch "acme/bin/$arch" "sys/lib/dist/bin/$arch"
while IFS= read -r dir; do
	mkdir -p "$arch/${dir#amd64/}"
done < <(find amd64/bin amd64/lib -type d)
cp adm/timezone/GMT adm/timezone/local
printf 'service=batch\n' >rc/bin/termrc
cp boot/rc/build.rc usr/glenda/batch/build.rc
printf '%s\n' "$arch" >.ci/target
touch .ci/console.log .ci/progress.log .ci/qemu.log

tail -n +1 -F .ci/console.log .ci/progress.log .ci/qemu.log &
logs=$!
ROOT=$PWD KERNEL=$PWD/.ci/kernel SMP=${SMP:-2} MEM=${MEM:-2G} SERIAL=file:$PWD/.ci/console.log \
	timeout -k 5 9000 boot/qemu -amd64 -nonet >.ci/qemu.log 2>&1 &
qemu=$!
cleanup() {
	kill "$qemu" "$logs" 2>/dev/null || :
	wait "$qemu" "$logs" 2>/dev/null || :
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

until grep -Eq 'BATCH-(PASS|FAIL)' .ci/console.log; do
	kill -0 "$qemu" 2>/dev/null || { echo 'QEMU exited before the build finished' >&2; exit 1; }
	sleep 2
done
grep -q 'BATCH-PASS' .ci/console.log || { echo 'build failed' >&2; exit 1; }
cleanup
trap - EXIT

tar --exclude="$arch/include" --exclude="$arch/mkfile" -czf ".ci/$arch.tar.gz" \
	"$arch" "acme/bin/$arch" "sys/lib/dist/bin/$arch"
echo "$arch: PASS .ci/$arch.tar.gz"
