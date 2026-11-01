#include <u.h>
#include <libc.h>

enum {
	Maxcpu	= 64,
	Niter	= 2000,
	Nfp	= 20000,
};

static long sem = 1;		/* binary semaphore; the kernel cmpswaps it */
static long count;		/* guarded by sem */
static long amocount;		/* ainc: AMOADD */

/* fill sys[i] with cpu i's syscall count; return the number of cpus */
static int
sysstat(vlong *sys)
{
	char buf[Maxcpu*128], *f[10], *p, *e;
	int fd, n, ncpu;

	fd = open("/dev/sysstat", OREAD);
	if(fd < 0)
		sysfatal("open /dev/sysstat: %r");
	n = readn(fd, buf, sizeof buf - 1);
	close(fd);
	if(n <= 0)
		sysfatal("read /dev/sysstat: %r");
	buf[n] = 0;
	ncpu = 0;
	for(p = buf; ncpu < Maxcpu && (e = strchr(p, '\n')) != nil; p = e+1){
		*e = 0;
		if(tokenize(p, f, nelem(f)) < 4 || atoi(f[0]) != ncpu)
			sysfatal("/dev/sysstat line %d", ncpu);
		sys[ncpu++] = strtoll(f[3], nil, 10);
	}
	if(ncpu == 0)
		sysfatal("/dev/sysstat: no cpus");
	return ncpu;
}

/* floating point state must survive yields to, and moves between, cpus */
static double
fpwork(int seed)
{
	double x;
	int j;

	x = seed;
	for(j = 0; j < Nfp; j++){
		x = x*1.0000001 + 0.25;
		if(j%256 == 0)
			sleep(0);
	}
	return x;
}

static void
waitall(int n, char *what)
{
	Waitmsg *w;

	while(n-- > 0){
		w = wait();
		if(w == nil)
			sysfatal("%s wait: %r", what);
		if(w->msg[0] != 0)
			sysfatal("%s child: %s", what, w->msg);
		free(w);
	}
}

/*
 * one child wired to each cpu increments a shared counter under a
 * semaphore and another with ainc; both totals must be exact, and each
 * cpu's syscall count must show its child ran there.  then twice as
 * many unwired children as cpus compute and yield, so they migrate.
 */
static int
smp(void)
{
	vlong before[Maxcpu], after[Maxcpu];
	int i, j, n, ncpu, fd;
	char ctl[32];
	double want;

	ncpu = sysstat(before);
	if(ncpu == 1)
		return 1;
	for(i = 0; i < ncpu; i++){
		switch(rfork(RFPROC|RFMEM)){
		case -1:
			sysfatal("rfork: %r");
		case 0:
			snprint(ctl, sizeof ctl, "/proc/%d/ctl", getpid());
			fd = open(ctl, OWRITE);
			if(fd < 0 || fprint(fd, "wired %d", i) < 0)
				sysfatal("wire to cpu%d: %r", i);
			close(fd);
			sleep(1);	/* wakes on cpu i; sleep(0) may not move us */
			for(j = 0; j < Niter; j++){
				while(semacquire(&sem, 1) < 0)
					;
				count++;
				if(semrelease(&sem, 1) < 0)
					sysfatal("semrelease: %r");
				ainc(&amocount);
			}
			exits(nil);
		}
	}
	waitall(ncpu, "smp");
	if(count != ncpu*Niter || amocount != ncpu*Niter || sem != 1)
		sysfatal("smp: semaphore count %ld, ainc %ld, sem %ld, want %d",
			count, amocount, sem, ncpu*Niter);
	if(sysstat(after) != ncpu)
		sysfatal("smp: cpu count changed");
	for(i = 0; i < ncpu; i++){
		n = after[i] - before[i];
		if(n < 2*Niter)
			sysfatal("smp: cpu%d made only %d syscalls", i, n);
	}
	for(i = 0; i < 2*ncpu; i++){
		want = fpwork(i);
		switch(fork()){
		case -1:
			sysfatal("fork: %r");
		case 0:
			if(fpwork(i) != want)
				exits("fpu state lost");
			exits(nil);
		}
	}
	waitall(2*ncpu, "fpu");
	print("MAXION-SMP-PASS: %d cpus, %ld semaphore and %ld ainc increments, %d fpu procs\n",
		ncpu, count, amocount, 2*ncpu);
	return ncpu;
}

void
main(void)
{
	int ncpu;
	uchar *p;
	ulong i, n;
	int pid, fd;
	char buf[64], name[64], *text;
	Waitmsg *w;
	double x;
	vlong t;

	n = 16*1024*1024;
	p = malloc(n);
	if(p == nil)
		sysfatal("malloc: %r");
	for(i = 0; i < n; i++)
		p[i] = (i*17 + 0xa5) & 255;
	for(i = 0; i < n; i++)
		if(p[i] != ((i*17 + 0xa5) & 255))
			sysfatal("memory mismatch at %lud", i);
	pid = fork();
	if(pid < 0)
		sysfatal("fork: %r");
	if(pid == 0){
		p[0] = 0;
		exits(nil);
	}
	w = wait();
	if(w == nil || w->pid != pid || w->msg[0] != 0 || p[0] != 0xa5)
		sysfatal("fork/wait or address-space isolation");
	free(w);
	free(p);
	x = strtod("1.25", nil);
	if(x*4.0 != 5.0)
		sysfatal("floating point");
	text = "9front Maxion volatile RAM filesystem\n";
	snprint(name, sizeof name, "/tmp/maxion-test.%d", getpid());
	fd = create(name, ORDWR|ORCLOSE, 0600);
	if(fd < 0 || write(fd, text, strlen(text)) != strlen(text) ||
	   seek(fd, 0, 0) != 0 || readn(fd, buf, strlen(text)) != strlen(text) ||
	   memcmp(buf, text, strlen(text)) != 0)
		sysfatal("RAM filesystem: %r");
	close(fd);
	t = nsec();
	sleep(100);
	t = nsec() - t;
	if(t < 80000000LL || t > 2000000000LL)
		sysfatal("timer interval %lld ns", t);
	ncpu = smp();
	print("MAXION-SMOKE-PASS: 16MiB memory, fork isolation, FPU, RAM files, timer %lld ns, %d cpu%s\n",
		t, ncpu, ncpu == 1? "": "s");
	exits(nil);
}
