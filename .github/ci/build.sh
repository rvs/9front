#!/usr/bin/env bash
# Usage: build.sh [ARCH ...]; no arguments builds all targets below.
# KCI_WORK=/tmp/9front-ci KCI_JOBS=2 KCI_CPUS=1 KCI_MEMORY=2G KCI_TIMEOUT=7200
# Requires qemu-system-x86_64 and /dev/kvm.
# KCI_SOURCE defaults to this checkout; KCI_SEED defaults to ./amd64.tar.gz.
# Outputs: KCI_WORK/ARCH/{root,console.log,ARCH.tar.gz}; existing roots are refused.
set -euo pipefail
all=(spim arm arm64 amd64 386 power power64 mips riscv64)
case ${1:-} in
-h|--help) sed -n '2,6s/^# //p' "$0"; printf 'Architectures: %s\n' "${all[*]}"; exit 0 ;;
--list) printf '%s\n' "${all[@]}"; exit 0 ;;
esac
[[ $# != 1 || $1 != all ]] || set --
(( $# )) || set -- "${all[@]}"
targets=(); declare -A seen=()
for arch; do
	case $arch in spim|arm|arm64|amd64|386|power|power64|mips|riscv64) ;; *) echo "Unknown architecture: $arch" >&2; exit 1 ;; esac
	[[ -z ${seen[$arch]+yes} ]] || continue
	targets+=("$arch"); seen[$arch]=1
done
source=$(realpath "${KCI_SOURCE:-$(dirname "$0")/../..}")
work=$(realpath -m "${KCI_WORK:-/tmp/9front-ci}")
seed=$(realpath "${KCI_SEED:-amd64.tar.gz}")
jobs=${KCI_JOBS:-2}; cpus=${KCI_CPUS:-1}; memory=${KCI_MEMORY:-2G}; limit=${KCI_TIMEOUT:-7200}
[[ $jobs =~ ^[1-9][0-9]{0,6}$ && $cpus =~ ^[1-9][0-9]{0,6}$ &&
	$limit =~ ^[1-9][0-9]{0,6}$ && $memory =~ ^[1-9][0-9]*[MG]$ ]] ||
	{ echo 'Invalid build limits' >&2; exit 1; }
[[ -r /dev/kvm && -w /dev/kvm ]] || { echo '/dev/kvm is not accessible' >&2; exit 1; }
[[ $work != / && $work != "$source" && $work != *','* && $work != *$'\n'* ]] || exit 1
mkdir -p "$work"
mkdir "$work/.build.lock" || { echo "Workspace locked: $work" >&2; exit 1; }
pids=(); snapshot=; bootstrap=
cleanup() {
	local status=$?
	trap - EXIT INT TERM HUP
	for pid in "${pids[@]}"; do kill -TERM "$pid" 2>/dev/null || :; done
	for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || :; done

	[[ -z $snapshot ]] || rm -f "$snapshot"
	[[ -z $bootstrap ]] || rm -rf "$bootstrap"
	rmdir "$work/.build.lock"
	exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
for arch in "${targets[@]}"; do
	[[ ! -e $work/$arch && ! -L $work/$arch ]] || { echo "Refusing existing workspace: $work/$arch" >&2; exit 1; }
done
revision=$(git -C "$source" rev-parse HEAD)
git -C "$source" cat-file -e "$revision:sys/src/mkfile"
snapshot=$(mktemp "$work/.source.XXXXXX")
git -C "$source" archive "$revision" >"$snapshot"
bootstrap=$(mktemp -d "$work/.amd64.XXXXXX")
mkdir "$bootstrap/amd64"
seedid=$(sha256sum "$seed")
tar -xf "$seed" -C "$bootstrap" amd64/bin amd64/lib amd64/init amd64/9pc64cpuwait
chmod -R u+rwX "$bootstrap"

build() {
	arch=$1; dir=$work/$arch; root=$dir/root; qemu=; logpid=
	trap - EXIT INT TERM HUP
	finish() {
		local status=$?
		trap - EXIT INT TERM HUP
		if [[ -n $qemu ]]; then
			kill -TERM "$qemu" 2>/dev/null || :
			for ((i=0; i<5; i++)); do kill -0 "$qemu" 2>/dev/null || break; sleep 1; done
			kill -KILL "$qemu" 2>/dev/null || :
			wait "$qemu" 2>/dev/null || :
		fi
		if [[ -n $logpid ]]; then
			kill "$logpid" 2>/dev/null || :
			wait "$logpid" 2>/dev/null || :
		fi
		if (( status )); then
			echo fail >"$dir/result"
			echo "$arch: FAILED; last guest diagnostics:" >&2
			tail -n 40 "$root/.ci/build.log" "$root/.ci/progress.log" "$dir/console.log" >&2 || :
		fi
		exit "$status"
	}
	mkdir -p "$root"
	trap finish EXIT
	trap 'exit 130' INT
	trap 'exit 143' TERM HUP
	printf 'source=%s\nseed=%s\nseed_id=%s\n' "$revision" "$seed" "$seedid" >"$dir/metadata.txt"
	echo "$arch: preparing git tree and bootstrap binaries"
	tar -xf "$snapshot" -C "$root"
	cp "$root/adm/timezone/GMT" "$root/adm/timezone/local"
	cp -a "$bootstrap/amd64/bin" "$bootstrap/amd64/lib" "$bootstrap/amd64/init" "$root/amd64/"
	chmod -R u+rwX "$root"
	sha256sum "$0" "$bootstrap/amd64/9pc64cpuwait" >>"$dir/metadata.txt"
	mkdir "$root/.ci"
	mkdir -p "$root"/{bin,dev,env,fd,mnt,net,n,proc,root,srv,shr,tmp}
	while IFS= read -r path; do
		mkdir -p "$root/$arch/$path"
	done < <(find "$bootstrap/amd64" -type d -printf '%P\n')
	mkdir -p "$root/rc/bin/postscript" "$root/usr/glenda/tmp" "$root/usr/glenda/lib" "$root/sys/lib/dist/bin/$arch" "$root/acme/bin/$arch"
	: >"$root/rc/bin/termrc" # no network or graphics startup
	printf 'exec /bin/rc /.ci/run.rc\n' >"$root/usr/glenda/lib/profile"
	printf 'target=%s\ndirs=(' "$arch" >"$root/.ci/env.rc"
	while IFS= read -r line; do
		[[ ! $line =~ ^[[:space:]]*([a-zA-Z0-9]+)\\[[:space:]]*$ ]] || printf '%s ' "${BASH_REMATCH[1]}" >>"$root/.ci/env.rc"
	done <"$root/sys/src/9/mkfile"
	printf 'rva23)\n' >>"$root/.ci/env.rc"
	cat >"$root/.ci/run.rc" <<'RC'
@{
	while(){
		date
		ps -ar
		cat /dev/swap
		sleep 60
	}
} >/.ci/progress.log >[2=1] &
/bin/rc -be /.ci/build.rc >/.ci/build.log >[2=1]
if(~ $status '') echo pass >/.ci/result.tmp
if not echo fail >/.ci/result.tmp
mv /.ci/result.tmp /.ci/result
while() sleep 60
RC
	cat >"$root/.ci/build.rc" <<'RC'
rfork e
. /.ci/env.rc
objtype=amd64
path=(/amd64/bin /rc/bin .)
# parallel mk triggers mount reply mismatches
NPROC=1
ramfs -m /tmp
fn build {
	d=$1
	shift
	echo BUILD $d $*
	cd /sys/src/$d
	mk $*
}
tools=`{sed -n 's/^(AS|CC|LD)=//p' /$target/mkfile}
if(~ $target spim) tools=(va vc vl)
for(d in cmd/cc libmach cmd/6c cmd/6a cmd/6l cmd/$tools){
	build $d nuke
	build $d install
}
build cmd ar.install pcc.install nm.install size.install strip.install file.install
objtype=$target
build . release
for(k in $dirs){
	cd /sys/src/9/$k
	if(~ `{sed -n 's/^objtype=//p' mkfile} $target){
		confs=()
		for(c in `{sed -n 's/^CONFLIST=//p' mkfile}){
			if(test -f $c) confs=($confs $c)
			if not echo skipping missing configuration $k/$c
		}
		if(~ $k pc64 rva23) confs=($confs $k^cpuwait)
		for(c in $confs){
			echo KERNEL $k $c
			if(~ $c *cpuwait){
				cp `{echo $c | sed 's/cpuwait$//'} $c
				rm -f $c.bootfs.proto
			}
			mk 'CONF='^$c clean
			files=9$c
			switch($k){
			case bcm bcm64 kw omap teg2 rva23
				files=($files s9$c)
			case arm64 cycv imx8 lx2k
				files=($files 9$c.u)
			case xen
				files=($files 9$c.gz)
			}
			mk 'CONF='^$c $files
			if(~ $k rva23) files=($files 9$c.elf 9$c.elf.sym)
			# Some install recipes contact remote servers; copy locally instead.
			for(f in $files){
				test -s $f
				cp $f /$target/$f
			}
		}
	}
}
for(f in init bin/rc bin/mk bin/ls lib/libc.a lib/libmach.a lib/ape/libap.a)
	test -s /$target/$f
for(f in mkwnew spout winfs News Wiki)
	test -s /acme/bin/$target/$f
RC
	touch "$dir/console.log" "$root/.ci/build.log" "$root/.ci/progress.log"
	tail -n +1 -F "$dir/console.log" "$root/.ci/build.log" "$root/.ci/progress.log" &
	logpid=$!
	qemu-system-x86_64 -accel kvm -cpu host -m "$memory" -smp "$cpus" \
		-display none -monitor none -serial "file:$dir/console.log" -no-reboot \
		-kernel "$bootstrap/amd64/9pc64cpuwait" \
		-append 'console=0 user=glenda service=terminal nobootprompt=virtio9p' \
		-fsdev "local,id=root,path=$root,security_model=none" \
		-device virtio-9p-pci,fsdev=root,mount_tag=root,disable-legacy=on -nic none \
		>"$dir/qemu.log" 2>&1 &
	qemu=$!; started=$SECONDS; deadline=$((SECONDS + limit)); next=$((SECONDS + 60))
	while [[ ! -f $root/.ci/result ]]; do
		kill -0 "$qemu" && (( SECONDS < deadline )) || { echo "$arch: guest exited or timed out" >&2; return 1; }
		if (( SECONDS >= next )); then
			printf '%s: running %ss, build.log=%s bytes\n' "$arch" "$((SECONDS - started))" "$(stat -c %s "$root/.ci/build.log")"
			next=$((SECONDS + 60))
		fi
		sleep 2
	done
	[[ $(cat "$root/.ci/result") == pass ]] || return 1
	[[ -s $root/.ci/build.log ]] || return 1
	if grep -En '^mk:.*(exit status=|don.t know how to make|no recipe|error)' "$root/.ci/build.log"; then return 1; fi
	for item in init bin/rc bin/mk bin/ls lib/libc.a lib/libmach.a lib/ape/libap.a; do
		[[ -s $root/$arch/$item ]] || { echo "$arch: missing $item" >&2; return 1; }
	done
	tar --exclude="$arch/include" --exclude="$arch/mkfile" -czf "$dir/$arch.tar.gz" \
		-C "$root" "$arch" "acme/bin/$arch" "sys/lib/dist/bin/$arch"
	echo pass >"$dir/result"
	echo "$arch: PASS $dir/$arch.tar.gz"
}

failed=0
for arch in "${targets[@]}"; do
	build "$arch" & pids+=("$!")
	if (( ${#pids[@]} >= jobs )); then
		for pid in "${pids[@]}"; do wait "$pid" || failed=1; done
		pids=()
	fi
done
for pid in "${pids[@]}"; do wait "$pid" || failed=1; done
pids=()
exit "$failed"
