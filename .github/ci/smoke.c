#include <u.h>
#include <libc.h>

enum {
	Nblock = 1024,
	Nproc = 8,
	Nio = 1024*1024,
	Chunk = 4096,
};

static void
check(int ok, char *what)
{
	if(!ok)
		sysfatal("%s: %r", what);
}

static void
reap(void)
{
	Waitmsg *w;

	w = wait();
	check(w != nil, "wait");
	check(w->msg[0] == 0, w->msg);
	free(w);
}

void
main(void)
{
	uchar *b[Nblock], buf[Chunk];
	char text[32];
	int i, j, n, total, pid, p[2], fd;
	uvlong v;
	double d;
	Dir *dir;

	v = 1ULL << 40;
	d = v;
	check((uvlong)d == v && v/7 == 157073089682ULL, "arithmetic");
	snprint(text, sizeof text, "%llud", v);
	check(strtoull(text, nil, 10) == v, "formatting");
	print("smoke: arithmetic ok\n");

	for(i = 0; i < Nblock; i++){
		b[i] = malloc(i+1);
		check(b[i] != nil, "malloc");
		memset(b[i], i, i+1);
	}
	for(i = 0; i < Nblock; i++)
		for(j = 0; j <= i; j++)
			check(b[i][j] == (i&255), "memory");
	print("smoke: memory ok\n");

	for(i = 0; i < Nproc; i++){
		pid = fork();
		check(pid >= 0, "fork");
		if(pid == 0){
			b[0][0] = 255;
			v = 0;
			for(j = 0; j < 100000; j++)
				v += j;
			check(v == 4999950000ULL, "child arithmetic");
			exits(nil);
		}
	}
	for(i = 0; i < Nproc; i++)
		reap();
	check(b[0][0] == 0, "copy on write");
	for(i = 0; i < Nblock; i++)
		free(b[i]);
	print("smoke: processes ok\n");

	for(i = 0; i < Chunk; i++)
		buf[i] = i;
	check(pipe(p) == 0, "pipe");
	pid = fork();
	check(pid >= 0, "fork pipe");
	if(pid == 0){
		close(p[0]);
		for(i = 0; i < Nio; i += Chunk)
			check(write(p[1], buf, Chunk) == Chunk, "write pipe");
		exits(nil);
	}
	close(p[1]);
	total = 0;
	while((n = read(p[0], buf, Chunk)) > 0){
		for(i = 0; i < n; i++)
			check(buf[i] == ((total+i)&255), "pipe data");
		total += n;
	}
	check(n == 0 && total == Nio, "read pipe");
	close(p[0]);
	reap();
	print("smoke: pipes ok\n");

	fd = create("/tmp/smoke.data", ORDWR, 0600);
	check(fd >= 0, "create");
	for(i = 0; i < Chunk; i++)
		buf[i] = i;
	for(i = 0; i < Nio; i += Chunk)
		check(write(fd, buf, Chunk) == Chunk, "write file");
	dir = dirfstat(fd);
	check(dir != nil && dir->length == Nio, "stat file");
	free(dir);
	check(seek(fd, 0, 0) == 0, "seek");
	for(i = 0; i < Nio; i += Chunk){
		check(readn(fd, buf, Chunk) == Chunk, "read file");
		for(j = 0; j < Chunk; j++)
			check(buf[j] == (j&255), "file data");
	}
	close(fd);
	check(remove("/tmp/smoke.data") == 0, "remove file");
	print("smoke: files ok\n");
	exits(nil);
}
