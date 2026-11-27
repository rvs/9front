#include "sys9.h"

typedef unsigned long long uvlong;
typedef long long vlong;
typedef unsigned char uchar;

static uvlong order = 0x0001020304050607ULL;

static void
be2vlong(vlong *to, uchar *f)
{
	uchar *t, *o;
	int i;

	t = (uchar*)to;
	o = (uchar*)&order;
	for(i = 0; i < 8; i++)
		t[o[i]] = f[i];
}

static char bintime[] = "/dev/bintime";

/* the program may have closed or dup2ed over the cached fd */
static int
stillopen(int fd)
{
	char buf[64];
	int i;

	if(fd < 0 || _FD2PATH(fd, buf, sizeof buf) < 0)
		return 0;
	for(i = 0; bintime[i] != 0; i++)
		if(buf[i] != bintime[i])
			return 0;
	return buf[i] == 0;
}

long long
_NSEC(void)
{
	uchar b[8];
	vlong t;
	int opened;
	static int fd = -1;

	opened = 0;
	if(!stillopen(fd))
		fd = -1;
	for(;;) {
		if(fd < 0)
			if(opened++ ||
			    (fd = _OPEN(bintime, OREAD|OCEXEC)) < 0)
				return 0;
		if(_PREAD(fd, b, sizeof b, 0) == sizeof b)
			break;		/* leave fd open for future use */
		/* short read, perhaps try again */
		_CLOSE(fd);
		fd = -1;
	}
	be2vlong(&t, b);
	return t;
}
