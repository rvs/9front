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
mkdir -p {bin,dev,env,fd,mnt,net,n,proc,root,srv,shr,tmp,lib/ndb}
mkdir -p rc/bin/postscript usr/glenda/tmp usr/glenda/batch "acme/bin/$arch" "sys/lib/dist/bin/$arch"
while IFS= read -r dir; do
	mkdir -p "$arch/${dir#amd64/}"
done < <(find amd64/bin amd64/lib -type d)
cp -R sys/lib/dist/ndb/. lib/ndb/
cp adm/timezone/US_Pacific adm/timezone/local
printf 'service=batch\n' >rc/bin/termrc
printf '%s\n' "$arch" >.ci/target
printf '%s\n' \
	'@{while(){date; ps -ar; cat /dev/swap; sleep 60}} >/.ci/progress.log >[2=1] &' \
	'NPROC=1' \
	'/usr/glenda/bin/rc/buildarch `{cat /.ci/target}' >usr/glenda/batch/build.rc
touch .ci/console.log .ci/progress.log .ci/qemu.log

tail -n +1 -F .ci/console.log .ci/progress.log .ci/qemu.log &
logs=$!
# build sets NPROC from the CPU count; parallel 9p requests can corrupt replies.
ROOT=$PWD KERNEL=$PWD/.ci/kernel SMP=1 MEM=${MEM:-2G} SERIAL=file:$PWD/.ci/console.log \
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

for f in "$arch"/9* "$arch"/s9*; do
	[[ ! -f $f || -s $f ]]
done
for f in init bin/rc bin/mk bin/ls lib/libc.a lib/libmach.a lib/ape/libap.a; do
	test -s "$arch/$f"
done
for f in mkwnew spout winfs News Wiki; do
	test -s "acme/bin/$arch/$f"
done

tar --exclude="$arch/include" --exclude="$arch/mkfile" -czf ".ci/$arch.tar.gz" \
	"$arch" "acme/bin/$arch" "sys/lib/dist/bin/$arch" lib/ndb adm/timezone/local
echo "$arch: PASS .ci/$arch.tar.gz"
