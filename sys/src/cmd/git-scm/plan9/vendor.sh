#!/bin/bash
# vendor the patched git tree into 9front's /sys/src/cmd/git-scm
set -e
SRC=${SRC:-$(cd "$(dirname "$0")/.." && pwd)}
OUT=${1:?usage: vendor.sh /path/to/9front/sys/src/cmd/git-scm}
D=/tmp/gitv
rm -rf $D; mkdir -p $D
git -C $SRC archive HEAD | tar x -C $D
cd $D
M="make uname_S=Plan9 uname_M=amd64 uname_O=Plan9 prefix=/sys/lib/git-scm gitexecdir=/bin/git-scm bindir=/bin/git-scm V=1 CC=pcc CFLAGS= DEVELOPER= NO_PERL=1 NO_PYTHON=1 NO_TCLTK=1 NO_GETTEXT=1 SHELL_PATH=/bin/sh DEFAULT_PAGER=cat DEFAULT_EDITOR=ed"
PROGS="git git-shell git-sh-i18n--envsubst git-daemon git-http-backend git-imap-send"
$M -n $PROGS 2>/dev/null | grep '^pcc' >/tmp/vcmds.txt
SCRIPTS="$(grep '^SCRIPT_SH +=' Makefile | sed 's/.*= *//; s/\.sh$//') git-sh-setup git-sh-i18n git-mergetool--lib"
$M -o git $SCRIPTS >/tmp/vmake.log 2>&1
$M -C templates SHELL_PATH=/bin/sh >>/tmp/vmake.log 2>&1
case "$OUT" in
*/cmd/git-scm) ;;
*) echo "vendor.sh: $OUT is not a .../cmd/git-scm directory" >&2; exit 1 ;;
esac
rm -rf "$OUT"; mkdir -p "$OUT"
python3 "$SRC/plan9/genmk.py" $D "$OUT" "$SCRIPTS"
