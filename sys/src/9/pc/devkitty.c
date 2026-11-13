#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/error.h"

#define Image IMAGE
#include <draw.h>
#include <memdraw.h>
#include <cursor.h>
#include "screen.h"
#include "../port/kitty.h"

enum {
	Maxwidth = 1920,
	Maxheight = 1200,
	Maxtiles = 1024,
	Tilesize = 64,
	Maxcell = 128,
	Maxside = Tilesize+Maxcell,
	Firstimage = 1001,

	Chunk = 3072,

	Qdir = 0,
	Qkbd,
	Qctl,
};

static struct {
	Lock;
	Rendez	r;
	Uart	*uart;
	Queue	*kbd;
	Ref	reader;
	Kitty	input;
	int	active;
	int	ready;
	int	probe;
	int	keyboard;
	int	keymode;
	int	resize;
	int	reset;
	int	ack;
	ulong	acktime;
	int	width;
	int	height;
	int	cellw;
	int	cellh;
	int	newwidth;
	int	newheight;
	int	newcellw;
	int	newcellh;
	int	tilew;
	int	tileh;
	int	nx;
	int	ny;
	int	next;
	uchar	dirty[Maxtiles];
	uchar	*pixels;
	char	*encoded;
	ulong	tiles;
	uvlong	bytes;
	ulong	keys;
	ulong	dropped;
	char	*status;
} kitty;

extern VGAdev vgakittydev;
extern VGAcur vgasoftcur;

static Dirtab kittydir[] = {
	".",		{Qdir, 0, QTDIR},	0,	0550,
	"kbd",		{Qkbd},			0,	0440,
	"kittyctl",	{Qctl},			0,	0660,
};

static ulong
now(void)
{
	return TK2MS(MACHP(0)->ticks);
}

static void
alltiles(void)
{
	memset(kitty.dirty, 1, kitty.nx*kitty.ny);
}

/* called with the console or draw locks held, sometimes at interrupt level */
void
kittyflush(VGAscr*, Rectangle r)
{
	int x, y;

	ilock(&kitty);
	if(kitty.active && kitty.tilew != 0
	&& rectclip(&r, Rect(0, 0, kitty.width, kitty.height))){
		for(y=r.min.y/kitty.tileh; y<=(r.max.y-1)/kitty.tileh; y++)
			for(x=r.min.x/kitty.tilew; x<=(r.max.x-1)/kitty.tilew; x++)
				kitty.dirty[y*kitty.nx+x] = 1;
	}
	iunlock(&kitty);
	wakeup(&kitty.r);
}

static void
kittykey(void*, Rune r, int down)
{
	char msg[UTFmax+2];
	int n;

	msg[0] = down ? 'r' : 'R';
	n = 1+runetochar(msg+1, &r);
	msg[n++] = 0;
	if(qproduce(kitty.kbd, msg, n) <= 0)
		kitty.dropped++;
	kitty.keys++;
}

static void
kittytext(void*, Rune r)
{
	char msg[UTFmax+2];
	int n;

	msg[0] = 'c';
	n = 1+runetochar(msg+1, &r);
	msg[n++] = 0;
	if(qproduce(kitty.kbd, msg, n) <= 0)
		kitty.dropped++;
	kitty.keys++;
}

static void
kittymouse(void*, int x, int y, int buttons, int pixel)
{
	int cw, ch;

	ilock(&kitty);
	cw = kitty.cellw;
	ch = kitty.cellh;
	iunlock(&kitty);
	if(!pixel){
		if(x > Maxwidth)
			x = Maxwidth;
		if(y > Maxheight)
			y = Maxheight;
		x = (x-1)*cw+cw/2;
		y = (y-1)*ch+ch/2;
	}
	absmousetrack(x, y, buttons, now());
}

static void
kittyresize(void*, int rows, int cols, int width, int height)
{
	int cw, ch;

	cw = kitty.input.cellw;
	ch = kitty.input.cellh;
	ilock(&kitty);
	if(cw > 0 && cw <= Maxcell)
		kitty.newcellw = cw;
	if(ch > 0 && ch <= Maxcell)
		kitty.newcellh = ch;
	/* row counts lose the partial cell at the bottom and right */
	if(width == 0)
		width = kitty.input.width;
	if(height == 0)
		height = kitty.input.height;
	if(width == 0 && cols > 0 && cols <= Maxwidth)
		width = cols*kitty.newcellw;
	if(height == 0 && rows > 0 && rows <= Maxheight)
		height = rows*kitty.newcellh;
	if(width >= 128)
		kitty.newwidth = width > Maxwidth ? Maxwidth : width;
	if(height >= 128)
		kitty.newheight = height > Maxheight ? Maxheight : height;
	kitty.resize = 1;
	iunlock(&kitty);
	wakeup(&kitty.r);
}

static void
kittyreply(void*, int kind, int value)
{
	ilock(&kitty);
	switch(kind){
	case Kittyack:
		kitty.ack = 0;
		break;
	case Kittygraphics:
		kitty.ready = value;
		kitty.status = value ? "ready" : "graphics unsupported";
		if(value)
			alltiles();
		break;
	case Kittykeyboard:
		kitty.input.keyboard = 1;
		kitty.keyboard = 1;
		break;
	case Kittypixel:
		kitty.input.pixel = value;
		break;
	}
	iunlock(&kitty);
	wakeup(&kitty.r);
}

static int
inputready(void*)
{
	return qlen(kitty.uart->iq) != 0;
}

static void
kittyin(void*)
{
	uchar buf[1024];
	int n, idle, reset;

	idle = 0;
	for(;;){
		if(waserror()){
			tsleep(&up->sleep, return0, nil, 100);
			continue;
		}
		ilock(&kitty);
		reset = kitty.reset;
		kitty.reset = 0;
		iunlock(&kitty);
		if(reset){
			kittyreset(&kitty.input);
			kitty.input.keyboard = 0;
			/* fresh replies must arrive after the parser is reset */
			ilock(&kitty);
			kitty.probe = 1;
			iunlock(&kitty);
			wakeup(&kitty.r);
		}
		if((n = qconsume(kitty.uart->iq, buf, sizeof buf)) > 0){
			kittyinput(&kitty.input, buf, n);
			idle = 0;
		}else{
			tsleep(&up->sleep, inputready, nil, 10);
			if((idle += 10) >= 50){
				kittytick(&kitty.input);
				idle = 0;
			}
		}
		poperror();
	}
}

static void
put(char *p, int n)
{
	qwrite(kitty.uart->oq, p, n);
	ilock(&kitty);
	kitty.bytes += n;
	iunlock(&kitty);
}

static void
puts(char *p)
{
	put(p, strlen(p));
}

static void
deleteimages(int n)
{
	char buf[80];
	int i;

	for(i=0; i<n; i++){
		snprint(buf, sizeof buf, "\033_Ga=d,d=I,i=%d,q=2\033\\", Firstimage+i);
		puts(buf);
	}
}

static void
probe(void)
{
	char buf[256];

	snprint(buf, sizeof buf, "\033[?1049h\033[?25l\033[?7l"
		"\033[?1003h\033[?1006h\033[?1016h"
		"\033[?1004h\033[?2004h\033[?2048h"
		"\033[14t\033[16t\033[18t\033[?u\033[?1016$p"
		"\033_Ga=q,i=%d,s=1,v=1,f=24,t=d;AAAA\033\\", Kittyquery);
	puts(buf);
}

/* the writer alone changes geometry and owns the image staging buffers */
static void
resize(void)
{
	VGAscr *scr;
	int w, h, cw, ch, n, screen;

	ilock(&kitty);
	w = kitty.newwidth;
	h = kitty.newheight;
	cw = kitty.newcellw;
	ch = kitty.newcellh;
	screen = w != kitty.width || h != kitty.height;
	kitty.resize = 0;
	if(!screen && cw == kitty.cellw && ch == kitty.cellh){
		iunlock(&kitty);
		return;
	}
	n = kitty.nx*kitty.ny;
	iunlock(&kitty);
	deleteimages(n);

	scr = &vgascreen[0];
	if(screen){
		qlock(&drawlock);
		if(waserror()){
			qunlock(&drawlock);
			nexterror();
		}
		cursoroff();
		deletescreenimage();
		setscreensize(scr, w, h, 32, XRGB32, 0);
		vgascreenwin(scr);
		vgasoftcur.enable(scr);
		setcursor(&cursor);
		cursoron();
		resetscreenimage();
		qunlock(&drawlock);
		poperror();
	}
	ilock(&kitty);
	kitty.width = w;
	kitty.height = h;
	kitty.cellw = cw;
	kitty.cellh = ch;
	kitty.tilew = (Tilesize+cw-1)/cw*cw;
	kitty.tileh = (Tilesize+ch-1)/ch*ch;
	kitty.nx = (w+kitty.tilew-1)/kitty.tilew;
	kitty.ny = (h+kitty.tileh-1)/kitty.tileh;
	kitty.next = 0;
	memset(kitty.dirty, 0, sizeof kitty.dirty);
	alltiles();
	iunlock(&kitty);
	if(screen)
		mouseresize();
}

static int
work(void*)
{
	int i, yes;

	ilock(&kitty);
	yes = kitty.probe || kitty.resize || (kitty.keyboard && !kitty.keymode);
	if(!yes && kitty.ready && !kitty.ack){
		for(i=0; i<kitty.nx*kitty.ny; i++)
			if(kitty.dirty[i]){
				yes = 1;
				break;
			}
	}
	iunlock(&kitty);
	return yes;
}

static int
snapshot(Rectangle *rp)
{
	VGAscr *scr;
	Memimage *m;
	Rectangle r;
	uchar *p, *q;
	int i, j, n, x, y;

	ilock(&kitty);
	n = kitty.nx*kitty.ny;
	i = kitty.next;
	for(j=0; j<n; j++){
		if(kitty.dirty[i])
			break;
		if(++i == n)
			i = 0;
	}
	if(j == n || !kitty.ready || kitty.ack){
		iunlock(&kitty);
		return -1;
	}
	kitty.dirty[i] = 0;
	kitty.next = (i+1)%n;
	r = Rect(i%kitty.nx*kitty.tilew, i/kitty.nx*kitty.tileh, 0, 0);
	r.max.x = r.min.x+kitty.tilew;
	r.max.y = r.min.y+kitty.tileh;
	rectclip(&r, Rect(0, 0, kitty.width, kitty.height));
	iunlock(&kitty);

	scr = &vgascreen[0];
	qlock(&drawlock);
	lock(&vgascreenlock);
	m = scr->gscreen;
	if(m == nil || m->chan != XRGB32 || !rectinrect(r, m->r)
	|| Dx(r) > Maxside || Dy(r) > Maxside){
		unlock(&vgascreenlock);
		qunlock(&drawlock);
		return -1;
	}
	q = kitty.pixels;
	for(y=r.min.y; y<r.max.y; y++){
		p = byteaddr(m, Pt(r.min.x, y));
		for(x=r.min.x; x<r.max.x; x++, p+=4){
			*q++ = p[2];
			*q++ = p[1];
			*q++ = p[0];
		}
	}
	unlock(&vgascreenlock);
	qunlock(&drawlock);
	*rp = r;
	return i;
}

static int
base64(char *out, uchar *p, int n)
{
	static char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	int i, v;
	char *q;

	q = out;
	for(i=0; i<n; i+=3){
		v = p[i]<<16;
		if(i+1 < n)
			v |= p[i+1]<<8;
		if(i+2 < n)
			v |= p[i+2];
		*q++ = alphabet[v>>18];
		*q++ = alphabet[v>>12&63];
		*q++ = i+1 < n ? alphabet[v>>6&63] : '=';
		*q++ = i+2 < n ? alphabet[v&63] : '=';
	}
	return q-out;
}

static void
sendtile(int id, Rectangle r)
{
	char buf[192];
	uchar *p;
	int n, m, first, cw, ch;

	ilock(&kitty);
	cw = kitty.cellw;
	ch = kitty.cellh;
	kitty.ack = 1;
	kitty.acktime = now();
	iunlock(&kitty);
	snprint(buf, sizeof buf, "\033[%d;%dH", r.min.y/ch+1, r.min.x/cw+1);
	puts(buf);
	p = kitty.pixels;
	n = Dx(r)*Dy(r)*3;
	first = 1;
	while(n > 0){
		m = n > Chunk ? Chunk : n;
		if(first){
			snprint(buf, sizeof buf,
				"\033_Ga=T,t=d,f=24,i=%d,p=1,s=%d,v=%d,C=1,q=2,m=%d;",
				Firstimage+id, Dx(r), Dy(r), n>m);
			first = 0;
		}else
			snprint(buf, sizeof buf, "\033_Gm=%d;", n>m);
		puts(buf);
		put(kitty.encoded, base64(kitty.encoded, p, m));
		puts("\033\\");
		p += m;
		n -= m;
	}
	puts("\033[5n");
	ilock(&kitty);
	kitty.tiles++;
	/* measure acknowledgement time from the end of a slow write */
	kitty.acktime = now();
	iunlock(&kitty);
}

static void
kittyout(void*)
{
	Rectangle r;
	int id, doprobe, keymode;
	ulong queried;

	queried = now();
	for(;;){
		if(waserror()){
			ilock(&kitty);
			kitty.ready = 0;
			kitty.status = "output error";
			iunlock(&kitty);
			tsleep(&kitty.r, return0, nil, 1000);
			continue;
		}
		tsleep(&kitty.r, work, nil, 100);
		ilock(&kitty);
		if(kitty.ack && now()-kitty.acktime > 5000){
			kitty.ack = 0;
			kitty.ready = 0;
			kitty.status = "acknowledgement timeout; write redraw to retry";
		}
		doprobe = kitty.probe;
		kitty.probe = 0;
		keymode = 0;
		if(doprobe){
			if(kitty.keymode)
				keymode = -1;
			kitty.keyboard = kitty.keymode = 0;
		}else if(kitty.keyboard && !kitty.keymode){
			keymode = 1;
			kitty.keymode = 1;
		}
		id = kitty.resize;
		iunlock(&kitty);
		if(keymode < 0)
			puts("\033[<u");
		if(doprobe)
			probe();
		/* older terminals do not report resizes in band */
		if(now()-queried > 2000){
			puts("\033[14t\033[16t\033[18t");
			queried = now();
		}
		if(keymode > 0)
			puts("\033[>31u");
		if(id)
			resize();
		if((id = snapshot(&r)) >= 0)
			sendtile(id, r);
		poperror();
	}
}

static void
kittyresetdev(void)
{
	if(getconf("kittyuart") == nil)
		return;
	kitty.kbd = qopen(16*1024, Qmsg, nil, nil);
	if(kitty.kbd == nil)
		panic("kitty keyboard");
	conf.monitor = 1;
}

static void
kittyinitdev(void)
{
	VGAscr *scr;
	char *s;

	if((s = getconf("kittyuart")) == nil)
		return;
	kitty.status = "initializing";
	if(waserror()){
		print("kitty: %s\n", up->errstr);
		kitty.status = "initialization failed";
		return;
	}
	kitty.pixels = smalloc(Maxside*Maxside*3);
	kitty.encoded = smalloc(Chunk/3*4);
	kitty.uart = uartclaim(s, nil);
	kittyinit(&kitty.input, nil);
	kitty.input.key = kittykey;
	kitty.input.text = kittytext;
	kitty.input.mouse = kittymouse;
	kitty.input.resize = kittyresize;
	kitty.input.reply = kittyreply;
	kitty.width = kitty.newwidth = 800;
	kitty.height = kitty.newheight = 600;
	kitty.cellw = kitty.newcellw = 8;
	kitty.cellh = kitty.newcellh = 16;
	kitty.tilew = kitty.tileh = Tilesize;
	kitty.nx = (kitty.width+Tilesize-1)/Tilesize;
	kitty.ny = (kitty.height+Tilesize-1)/Tilesize;

	scr = &vgascreen[0];
	qlock(&drawlock);
	if(waserror()){
		qunlock(&drawlock);
		nexterror();
	}
	deletescreenimage();
	scr->dev = &vgakittydev;
	scr->paddr = 0;
	scr->vaddr = nil;
	scr->softscreen = 1;
	setscreensize(scr, kitty.width, kitty.height, 32, XRGB32, 0);
	kitty.active = 1;
	vgascreenwin(scr);
	scr->cur = &vgasoftcur;
	vgasoftcur.enable(scr);
	resetscreenimage();
	qunlock(&drawlock);
	poperror();

	kitty.probe = 1;
	kitty.status = "negotiating";
	kproc("kittyin", kittyin, nil);
	kproc("kittyout", kittyout, nil);
	poperror();
}

static Chan*
kittyattach(char *spec)
{
	if(!kitty.active)
		error(Enodev);
	return devattach(L'κ', spec);
}

static Walkqid*
kittywalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, kittydir, nelem(kittydir), devgen);
}

static int
kittystat(Chan *c, uchar *dp, int n)
{
	return devstat(c, dp, n, kittydir, nelem(kittydir), devgen);
}

static Chan*
kittyopen(Chan *c, int omode)
{
	if(c->qid.path == Qkbd){
		if(incref(&kitty.reader) != 1){
			decref(&kitty.reader);
			error(Einuse);
		}
		if(waserror()){
			decref(&kitty.reader);
			nexterror();
		}
		c = devopen(c, omode, kittydir, nelem(kittydir), devgen);
		poperror();
		return c;
	}
	return devopen(c, omode, kittydir, nelem(kittydir), devgen);
}

static void
kittyclose(Chan *c)
{
	if((c->flag & COPEN) && c->qid.path == Qkbd)
		decref(&kitty.reader);
}

static long
kittyread(Chan *c, void *a, long n, vlong off)
{
	char buf[384];

	switch((ulong)c->qid.path){
	case Qdir:
		return devdirread(c, a, n, kittydir, nelem(kittydir), devgen);
	case Qkbd:
		if(n < UTFmax+2)
			error(Eshort);
		return qread(kitty.kbd, a, n);
	case Qctl:
		ilock(&kitty);
		snprint(buf, sizeof buf, "%s\nsize %dx%d cell %dx%d\n"
			"tiles %lud bytes %llud keys %lud dropped %lud\n",
			kitty.status, kitty.width, kitty.height, kitty.cellw, kitty.cellh,
			kitty.tiles, kitty.bytes, kitty.keys, kitty.dropped);
		iunlock(&kitty);
		return readstr(off, a, n, buf);
	}
	error(Egreg);
}

static long
kittywrite(Chan *c, void *a, long n, vlong)
{
	Cmdbuf *cb;

	if(c->qid.path != Qctl)
		error(Egreg);
	cb = parsecmd(a, n);
	if(waserror()){
		free(cb);
		nexterror();
	}
	if(cb->nf != 1 || strcmp(cb->f[0], "redraw") != 0)
		error(Ebadctl);
	ilock(&kitty);
	kitty.ready = 0;
	kitty.ack = 0;
	kitty.reset = 1;
	kitty.status = "negotiating";
	alltiles();
	iunlock(&kitty);
	wakeup(&kitty.r);
	poperror();
	free(cb);
	return n;
}

Dev kittydevtab = {
	L'κ',
	"kitty",

	kittyresetdev,
	kittyinitdev,
	devshutdown,
	kittyattach,
	kittywalk,
	kittystat,
	kittyopen,
	devcreate,
	kittyclose,
	kittyread,
	devbread,
	kittywrite,
	devbwrite,
	devremove,
	devwstat,
};
