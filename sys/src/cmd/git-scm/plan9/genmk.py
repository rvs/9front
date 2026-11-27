# generate /sys/src/cmd/git-scm from a configured git tree
import os, re, shlex, shutil, sys

D, OUT, SCRIPTS = sys.argv[1], sys.argv[2], sys.argv[3].split()

recs = []
links = {}
for l in open('/tmp/vcmds.txt'):
	a = shlex.split(l)
	o = a[a.index('-o')+1]
	if '-c' not in a:
		links[o] = [x for x in a[1:] if x.endswith('.o') or x.endswith('.a')]
		continue
	flags = [x for x in a[1:-1] if x not in ('-o', o, '-c')]
	recs.append((o, a[-1], flags))
seen = set(); u = []
for r in recs:
	if r[0] not in seen:
		seen.add(r[0]); u.append(r)
recs = u

drop = lambda x: x.startswith('-DGIT_HOST_CPU=') or x.startswith('-DSHELL_PATH=')
common = set(recs[0][2])
for r in recs:
	common &= set(r[2])
basic = [x for x in recs[0][2] if x in common and not drop(x)]

progs = {'git': 'git.o', 'git-shell': 'shell.o', 'git-sh-i18n--envsubst': 'sh-i18n--envsubst.o',
	'git-daemon': 'daemon.o', 'git-http-backend': 'http-backend.o', 'git-imap-send': 'imap-send.o'}
progs = {p: o for p, o in progs.items() if p in links}
mains = set(progs.values()) | {'common-main.o'}
objs = [o for o, s, f in recs if o not in mains] + ['plan9/compat.o']

# files
def cp(rel):
	d = os.path.join(OUT, os.path.dirname(rel))
	os.makedirs(d, exist_ok=True)
	shutil.copy2(os.path.join(D, rel), os.path.join(OUT, rel))
srcs = [s for o, s, f in recs] + ['plan9/compat.c']
skip = re.compile(r'^(t|contrib|Documentation|po|gitk-git|git-gui|perl|ci|templates|tools|xdiff/.*test.*)/')
hdrs = []
for root, ds, fs in os.walk(D):
	for f in fs:
		rel = os.path.relpath(os.path.join(root, f), D)
		if f.endswith('.h') and not skip.match(rel):
			hdrs.append(rel)
done = set()
todo = srcs + hdrs
while todo:
	rel = todo.pop()
	if rel in done:
		continue
	done.add(rel)
	cp(rel)
	for m in re.finditer(r'#\s*include\s+"([^"]+\.c)"', open(os.path.join(D, rel), errors='replace').read()):
		inc = os.path.normpath(os.path.join(os.path.dirname(rel), m.group(1)))
		if os.path.exists(os.path.join(D, inc)):
			todo.append(inc)
shutil.copy2(os.path.join(os.path.dirname(sys.argv[0]), 'README.git-scm'), OUT + '/README')
for f in ('COPYING', 'LGPL-2.1', 'README.md', 'plan9/vendor.sh', 'plan9/genmk.py'):
	cp(f)
os.makedirs(OUT + '/scripts')
for s in SCRIPTS:
	t = open(os.path.join(D, s)).read()
	t = re.sub(r'^#!/bin/sh', '#!/bin/ape/sh', t)
	open(os.path.join(OUT, 'scripts', s), 'w').write(t)
shutil.copytree(D + '/templates/blt', OUT + '/templates')
for root, ds, fs in os.walk(OUT + '/templates'):
	for f in fs:
		p = os.path.join(root, f)
		t = open(p).read()
		open(p, 'w').write(re.sub(r'^#!/bin/sh', '#!/bin/ape/sh', t))

# mkfile
O = lambda x: re.sub(r'\.o$', '.$O', x)
q = lambda x: "'" + x.replace("'", "''") + "'"
m = []
m.append('</$objtype/mkfile')
m.append('')
m.append('BIN=/$objtype/bin/git-scm')
m.append('LIB=/sys/lib/git-scm')
m.append('CC=pcc')
m.append('LD=pcc')
m.append('ENDIAN=`{if(~ $objtype mips power power64 sparc) echo -D_BIG_ENDIAN -DSHA1DC_FORCE_BIGENDIAN; if not echo -D_LITTLE_ENDIAN}')
m.append('APEFLAGS=-+ -B -D_POSIX_SOURCE -D_POSIX_C_SOURCE=200112L -D_BSD_EXTENSION -D_SUSV2_SOURCE\\\n\t-D_C99_SNPRINTF_EXTENSION -D_LIMITS_EXTENSION -D_REENTRANT_SOURCE -D_RESEARCH_SOURCE\\\n\t-D__STDC_VERSION__=199901L -D__plan9__ $ENDIAN -DHAVE_SOCK_OPTS -Dtypestr=typestr_')
m.append("GITFLAGS=-Iplan9 '-DCSPRNG_DEVICE=\"/dev/random\"' '-DSHELL_PATH=\"/bin/ape/sh\"'\\\n\t" + ' \\\n\t'.join(q(x) for x in basic))
m.append('CFLAGS=$APEFLAGS $GITFLAGS')
m.append('')
m.append('PROGS=' + ' '.join(sorted(progs)))
m.append('SCRIPTS=\\\n\t' + ' \\\n\t'.join(SCRIPTS))
tf=[]; td=['templates']
for root, ds, fs in os.walk(OUT + '/templates'):
	for d in ds: td.append(os.path.relpath(os.path.join(root, d), OUT))
	for f in fs: tf.append(os.path.relpath(os.path.join(root, f), OUT))
m.append('TMPLDIRS=' + ' '.join(sorted(td)))
m.append('TMPLFILES=\\\n\t' + ' \\\n\t'.join(sorted(tf)))
m.append('DASHED=upload-pack receive-pack upload-archive')
m.append('OFILES=\\\n\t' + ' \\\n\t'.join(O(o) for o in objs))
m.append('')
m.append('all:V: $PROGS')
m.append('')
for p, o in sorted(progs.items()):
	m.append('%s: %s common-main.$O libgit.a$O\n\t$LD -o $target $prereq -lz\n' % (p, O(o)))
m.append('libgit.a$O: $OFILES\n\tar vu $target $newprereq\n')
m.append('%.$O: %.c\n\t$CC $CFLAGS -c -o $target $stem.c\n')
for o, s, f in recs:
	ex = [x for x in f if x not in common and not drop(x)]
	if s == 'help.c':
		ex = ["'-DGIT_HOST_CPU=\"'$objtype'\"'"] + ex
		m.append("%s: %s\n\t$CC $CFLAGS %s -c -o $target %s\n" % (O(o), s, ' '.join(ex[:1]) + ' ' + ' '.join(q(x) for x in ex[1:]), s))
	elif ex:
		m.append('%s: %s\n\t$CC $CFLAGS %s -c -o $target %s\n' % (O(o), s, ' '.join(q(x) for x in ex), s))
m.append('''install:V: all
	mkdir -p $BIN $LIB/share/git-core
	for(i in $PROGS) cp $i $BIN/$i
	for(i in $SCRIPTS) {
		cp scripts/$i $BIN/$i
		chmod +x $BIN/$i
	}
	for(i in $DASHED) {
		echo '#!/bin/rc' >$BIN/git-$i
		echo 'exec /bin/git-scm/git '$i' $*' >>$BIN/git-$i
		chmod +x $BIN/git-$i
	}
	rm -rf $LIB/share/git-core/templates
	for(i in $TMPLDIRS) mkdir -p $LIB/share/git-core/$i
	for(i in $TMPLFILES) cp $i $LIB/share/git-core/$i

installall:V:
	for(objtype in $CPUS) mk $MKFLAGS install

clean:V:
	rm -f $PROGS libgit.a[$OS] *.[$OS] */*.[$OS] */*/*.[$OS]

nuke:V: clean
''')
open(OUT + '/mkfile', 'w').write('\n'.join(m))
print(len(done), 'source files;', len(objs), 'objects;', 'progs', sorted(progs))
