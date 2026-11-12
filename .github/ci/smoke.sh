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
case $arch in
386|amd64)
	qemu=(qemu-system-x86_64 -accel kvm -cpu host -kernel "$root/$arch/$kernel"
		-append 'console=0 user=glenda service=terminal nobootprompt=virtio9p')
	;;
arm64)
	printf 'console=0\nuser=glenda\nservice=terminal\nnobootprompt=virtio9p\n*maxmem=0x60000000\n*ncpu=1\n' >"$work/plan9.ini"
	qemu=(qemu-system-aarch64 -M virt,gic-version=3,highmem=off -cpu cortex-a72
		-kernel "$root/$arch/$kernel.u"
		-device "loader,file=$work/plan9.ini,addr=0x40010000,force-raw=on")
	;;
riscv64)
	qemu=(qemu-system-riscv64 -M virt -bios default -cpu rv64,svpbmt=true,zicbom=true,sstc=true
		-kernel "$root/$arch/$kernel"
		-append 'console=0 user=glenda service=terminal nobootprompt=virtio9p')
	;;
esac
SMOKE_LOG="$work/console.log" expect -f - "${qemu[@]}" \
	-m 512M -smp 1 -display none -monitor none -serial stdio -no-reboot \
	-fsdev "local,id=root,path=$root,security_model=none" \
	-device virtio-9p-pci,fsdev=root,mount_tag=root,disable-legacy=on \
	-device virtio-net-pci,netdev=net0,disable-legacy=on,romfile= -netdev user,id=net0 <<'EXPECT'
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
