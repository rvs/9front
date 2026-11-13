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
#include <flate.h>
#include "screen.h"
#include "../port/kitty.h"

enum {
	Maxwidth = 1920,
	Maxheight = 1200,
	Maxpwidth = 3840,
	Maxpheight = 2400,
	Maxtiles = 4096,
	Tilesize = 64,
	Maxcell = 128,
	Maxscale = 8,
	Hidpi = 28,
	Maxside = Tilesize+Maxcell,
	Firstimage = 1001,

	Chunk = 3072,
	Maxbatch = 16,

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
	int	probewait;
	int	quiet;
	int	query;
	ulong	generation;
	int	keyboard;
	int	keymode;
	int	resize;
	int	reset;
	int	ack;
	ulong	acktime;
	int	width;
	int	height;
	int	pwidth;
	int	pheight;
	int	scale;
	int	scaling;
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
	uchar	*packed;
	char	*encoded;
	int	compress;
	int	batch;
	ulong	ztiles;
	ulong	batches;
	uvlong	rawbytes;
	uvlong	packedbytes;
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

static int
scalefactor(int ch)
{
	int s;

	s = kitty.scaling;
	if(s == 1)
		return ch >= Hidpi ? 2 : 1;
	return s == 0 ? 1 : s;
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
		r.min.x *= kitty.scale;
		r.min.y *= kitty.scale;
		r.max.x *= kitty.scale;
		r.max.y *= kitty.scale;
		for(y=r.min.y/kitty.tileh; y<=(r.max.y-1)/kitty.tileh; y++)
			for(x=r.min.x/kitty.tilew; x<=(r.max.x-1)/kitty.tilew; x++)
				kitty.dirty[y*kitty.nx+x] = 1;
	}
	iunlock(&kitty);
	wakeup(&kitty.r);
}

static int
inputquiet(void)
{
	int quiet;

	ilock(&kitty);
	quiet = kitty.quiet;
	iunlock(&kitty);
	return quiet;
}

static void
kittykey(void*, Rune r, int down)
{
	char msg[UTFmax+2];
	int n;

	/* releases from the old parser must still reach kbdfs */
	if(down && inputquiet())
		return;
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

	if(inputquiet())
		return;
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
	int cw, ch, s;

	if(buttons != 0 && inputquiet())
		return;
	ilock(&kitty);
	cw = kitty.cellw;
	ch = kitty.cellh;
	s = kitty.scale;
	iunlock(&kitty);
	if(!pixel){
		if(x > Maxpwidth)
			x = Maxpwidth;
		if(y > Maxpheight)
			y = Maxpheight;
		x = (x-1)*cw+cw/2;
		y = (y-1)*ch+ch/2;
	}
	absmousetrack(x/s, y/s, buttons, now());
}

static void
kittyresize(void*, int rows, int cols, int width, int height)
{
	int cw, ch, s;

	cw = kitty.input.cellw;
	ch = kitty.input.cellh;
	ilock(&kitty);
	if(cw > 0 && cw <= Maxcell)
		kitty.newcellw = cw;
	if(ch > 0 && ch <= Maxcell)
		kitty.newcellh = ch;
	s = scalefactor(kitty.newcellh);
	/* row counts lose the partial cell at the bottom and right */
	if(width == 0)
		width = kitty.input.width;
	if(height == 0)
		height = kitty.input.height;
	if(width == 0 && cols > 0 && cols <= Maxpwidth)
		width = cols*kitty.newcellw;
	if(height == 0 && rows > 0 && rows <= Maxpheight)
		height = rows*kitty.newcellh;
	if(width/s >= 128){
		if(width > Maxpwidth)
			width = Maxpwidth;
		kitty.newwidth = width > Maxwidth*s ? Maxwidth*s : width;
	}
	if(height/s >= 128){
		if(height > Maxpheight)
			height = Maxpheight;
		kitty.newheight = height > Maxheight*s ? Maxheight*s : height;
	}
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
		if(kitty.ready)
			kitty.ack = 0;
		break;
	case Kittygraphics:
		if(kitty.probewait && kitty.input.query == kitty.query){
			kitty.probewait = 0;
			kitty.quiet = 0;
			kitty.ready = value;
			kitty.status = value ? "ready" : "graphics unsupported";
			if(value)
				alltiles();
		}
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
	int n, reset, query;
	ulong lastinput;

	lastinput = now();
	for(;;){
		if(waserror()){
			tsleep(&up->sleep, return0, nil, 100);
			continue;
		}
		ilock(&kitty);
		reset = kitty.reset;
		kitty.reset = 0;
		query = kitty.query;
		iunlock(&kitty);
		if(reset){
			kittyreset(&kitty.input);
			kitty.input.keyboard = 0;
			kitty.input.query = query;
			/* fresh replies must arrive after the parser is reset */
			ilock(&kitty);
			if(query == kitty.query && !kitty.reset)
				kitty.probe = 1;
			iunlock(&kitty);
			wakeup(&kitty.r);
		}
		if((n = qconsume(kitty.uart->iq, buf, sizeof buf)) > 0){
			kittyinput(&kitty.input, buf, n);
			lastinput = now();
		}else{
			if(now()-lastinput >= 50)
				kittytick(&kitty.input);
			tsleep(&up->sleep, inputready, nil, 2);
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
probe(int query)
{
	char buf[256];

	ilock(&kitty);
	if(query != kitty.query || kitty.reset){
		iunlock(&kitty);
		return;
	}
	kitty.probewait = 1;
	iunlock(&kitty);
	snprint(buf, sizeof buf, "\033[?1049h\033[?25l\033[?7l"
		"\033[?1003h\033[?1006h\033[?1016h"
		"\033[?1004h\033[?2004h\033[?2048h"
		"\033[14t\033[16t\033[18t\033[?u\033[?1016$p"
		"\033_Ga=q,i=%d,s=1,v=1,f=24,t=d;AAAA\033\\", query);
	puts(buf);
}

/* the writer alone changes geometry and owns the image staging buffers */
static void
resize(void)
{
	VGAscr *scr;
	int w, h, pw, ph, cw, ch, s, n, screen;

	ilock(&kitty);
	w = kitty.newwidth;
	h = kitty.newheight;
	cw = kitty.newcellw;
	ch = kitty.newcellh;
	s = scalefactor(ch);
	w /= s;
	h /= s;
	if(w > Maxwidth)
		w = Maxwidth;
	if(h > Maxheight)
		h = Maxheight;
	pw = w*s;
	ph = h*s;
	screen = w != kitty.width || h != kitty.height;
	kitty.resize = 0;
	if(w < 128 || h < 128
	|| (!screen && pw == kitty.pwidth && ph == kitty.pheight
	&& s == kitty.scale && cw == kitty.cellw && ch == kitty.cellh)){
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
	kitty.pwidth = pw;
	kitty.pheight = ph;
	kitty.scale = s;
	kitty.cellw = cw;
	kitty.cellh = ch;
	kitty.tilew = (Tilesize+cw-1)/cw*cw;
	kitty.tileh = (Tilesize+ch-1)/ch*ch;
	kitty.nx = (pw+kitty.tilew-1)/kitty.tilew;
	kitty.ny = (ph+kitty.tileh-1)/kitty.tileh;
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
	yes = kitty.probe || (kitty.resize && !kitty.ack)
		|| (kitty.keyboard && !kitty.keymode);
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
snapshot(Rectangle *rp, int *sent, int nsent)
{
	VGAscr *scr;
	Memimage *m;
	Rectangle r, src;
	uchar *p, *q;
	int i, j, k, n, x, y, s, left;

	ilock(&kitty);
	n = kitty.nx*kitty.ny;
	i = kitty.next;
	for(j=0; j<n; j++){
		if(kitty.dirty[i]){
			for(k=0; k<nsent; k++)
				if(sent[k] == i)
					break;
			if(k == nsent)
				break;
		}
		if(++i == n)
			i = 0;
	}
	if(j == n || !kitty.ready || kitty.ack || kitty.resize || kitty.reset){
		iunlock(&kitty);
		return -1;
	}
	kitty.dirty[i] = 0;
	kitty.next = (i+1)%n;
	r = Rect(i%kitty.nx*kitty.tilew, i/kitty.nx*kitty.tileh, 0, 0);
	r.max.x = r.min.x+kitty.tilew;
	r.max.y = r.min.y+kitty.tileh;
	rectclip(&r, Rect(0, 0, kitty.pwidth, kitty.pheight));
	s = kitty.scale;
	iunlock(&kitty);

	src = Rect(r.min.x/s, r.min.y/s,
		(r.max.x+s-1)/s, (r.max.y+s-1)/s);
	scr = &vgascreen[0];
	qlock(&drawlock);
	lock(&vgascreenlock);
	m = scr->gscreen;
	if(m == nil || m->chan != XRGB32 || !rectinrect(src, m->r)
	|| Dx(r) > Maxside || Dy(r) > Maxside){
		unlock(&vgascreenlock);
		qunlock(&drawlock);
		return -1;
	}
	q = kitty.pixels;
	for(y=r.min.y; y<r.max.y; y++){
		p = byteaddr(m, Pt(r.min.x/s, y/s));
		left = s-r.min.x%s;
		for(x=r.min.x; x<r.max.x; x++){
			*q++ = p[2];
			*q++ = p[1];
			*q++ = p[0];
			/* tile boundaries need not coincide with scaled pixels */
			if(--left == 0){
				p += 4;
				left = s;
			}
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
	int n, m, raw, z, first, cw, ch, compress;

	ilock(&kitty);
	cw = kitty.cellw;
	ch = kitty.cellh;
	compress = kitty.compress;
	iunlock(&kitty);
	p = kitty.pixels;
	raw = n = Dx(r)*Dy(r)*3;
	z = 0;
	if(compress){
		m = deflatezlibblock(kitty.packed, n-1, p, n, 1, 0);
		if(m > 0 && m < n){
			p = kitty.packed;
			n = m;
			z = 1;
		}
	}
	ilock(&kitty);
	kitty.rawbytes += raw;
	kitty.packedbytes += n;
	kitty.ztiles += z;
	iunlock(&kitty);
	snprint(buf, sizeof buf, "\033[%d;%dH", r.min.y/ch+1, r.min.x/cw+1);
	puts(buf);

	first = 1;
	while(n > 0){
		m = n > Chunk ? Chunk : n;
		if(first){
			snprint(buf, sizeof buf,
				"\033_Ga=T,t=d,f=24,i=%d,p=1,s=%d,v=%d,C=1,q=2,%sm=%d;",
				Firstimage+id, Dx(r), Dy(r), z ? "o=z," : "", n>m);
			first = 0;
		}else
			snprint(buf, sizeof buf, "\033_Gm=%d;", n>m);
		puts(buf);
		put(kitty.encoded, base64(kitty.encoded, p, m));
		puts("\033\\");
		p += m;
		n -= m;
	}
	ilock(&kitty);
	kitty.tiles++;
	iunlock(&kitty);
}

static void
kittyout(void*)
{
	Rectangle r;
	int id, doprobe, keymode, query, batch, n, sent[Maxbatch], fence;
	ulong queried, generation;

	queried = now();
	for(;;){
		if(waserror()){
			ilock(&kitty);
			kitty.ready = 0;
			kitty.ack = 0;
			kitty.probewait = 0;
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
		doprobe = kitty.probe && !kitty.reset;
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
		id = kitty.resize && !kitty.ack;
		query = kitty.query;
		batch = kitty.batch;
		generation = kitty.generation;
		iunlock(&kitty);
		if(keymode < 0)
			puts("\033[<u");
		if(doprobe)
			probe(query);
		/* older terminals do not report resizes in band */
		if(now()-queried > 2000){
			puts("\033[14t\033[16t\033[18t");
			queried = now();
		}
		if(keymode > 0)
			puts("\033[>31u");
		if(id)
			resize();
		for(n=0; n<batch; n++){
			if((id = snapshot(&r, sent, n)) < 0)
				break;
			sent[n] = id;
			sendtile(id, r);
		}
		fence = 0;
		if(n > 0){
			ilock(&kitty);
			if(kitty.ready && !kitty.reset && generation == kitty.generation){
				kitty.ack = 1;
				kitty.acktime = now();
				kitty.batches++;
				fence = 1;
			}
			iunlock(&kitty);
		}
		if(fence){
			puts("\033[5n");
			ilock(&kitty);
			/* a fast reply may already have cleared the fence */
			if(kitty.ack && generation == kitty.generation)
				kitty.acktime = now();
			iunlock(&kitty);
		}
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
	char *s, *p, *ep;

	if((s = getconf("kittyuart")) == nil)
		return;
	kitty.status = "initializing";
	if(waserror()){
		print("kitty: %s\n", up->errstr);
		kitty.status = "initialization failed";
		return;
	}
	if((p = getconf("kittyscale")) != nil){
		kitty.scaling = strtol(p, &ep, 10);
		if(*p == 0 || *ep != 0 || kitty.scaling < 0 || kitty.scaling > Maxscale)
			error("bad kittyscale");
	}
	kitty.pixels = smalloc(Maxside*Maxside*3);
	kitty.packed = smalloc(Maxside*Maxside*3);
	kitty.encoded = smalloc(Chunk/3*4);
	if(deflateinit() != FlateOk)
		error("kitty compression initialization");
	kitty.compress = 1;
	kitty.batch = 8;
	kitty.quiet = 1;
	kitty.query = Kittyquery;
	kitty.uart = uartclaim(s, nil);
	kittyinit(&kitty.input, nil);
	kitty.input.key = kittykey;
	kitty.input.text = kittytext;
	kitty.input.mouse = kittymouse;
	kitty.input.resize = kittyresize;
	kitty.input.reply = kittyreply;
	kitty.scale = scalefactor(16);
	kitty.width = 800/kitty.scale;
	kitty.height = 600/kitty.scale;
	if(kitty.width < 128)
		kitty.width = 128;
	if(kitty.height < 128)
		kitty.height = 128;
	kitty.pwidth = kitty.newwidth = kitty.width*kitty.scale;
	kitty.pheight = kitty.newheight = kitty.height*kitty.scale;
	kitty.cellw = kitty.newcellw = 8;
	kitty.cellh = kitty.newcellh = 16;
	kitty.tilew = kitty.tileh = Tilesize;
	kitty.nx = (kitty.pwidth+Tilesize-1)/Tilesize;
	kitty.ny = (kitty.pheight+Tilesize-1)/Tilesize;

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
	char buf[512];

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
			"tiles %lud bytes %llud keys %lud dropped %lud\n"
			"compress %d ztiles %lud raw %llud packed %llud batch %d groups %lud\n"
			"scale %d output %dx%d\n",
			kitty.status, kitty.width, kitty.height, kitty.cellw, kitty.cellh,
			kitty.tiles, kitty.bytes, kitty.keys, kitty.dropped,
			kitty.compress, kitty.ztiles, kitty.rawbytes, kitty.packedbytes,
			kitty.batch, kitty.batches, kitty.scale, kitty.pwidth, kitty.pheight);
		iunlock(&kitty);
		return readstr(off, a, n, buf);
	}
	error(Egreg);
}

static long
kittywrite(Chan *c, void *a, long n, vlong)
{
	Cmdbuf *cb;
	char *ep;
	int batch;

	if(c->qid.path != Qctl)
		error(Egreg);
	cb = parsecmd(a, n);
	if(waserror()){
		free(cb);
		nexterror();
	}
	if(cb->nf == 1 && strcmp(cb->f[0], "redraw") == 0){
		ilock(&kitty);
		kitty.ready = 0;
		kitty.ack = 0;
		kitty.probe = kitty.probewait = 0;
		kitty.quiet = 1;
		kitty.reset = 1;
		if(--kitty.query < 0x40000000)
			kitty.query = Kittyquery;
		kitty.generation++;
		kitty.status = "negotiating";
		alltiles();
		iunlock(&kitty);
	}else if(cb->nf == 2 && strcmp(cb->f[0], "compress") == 0){
		if(strcmp(cb->f[1], "0") != 0 && strcmp(cb->f[1], "1") != 0)
			error(Ebadctl);
		ilock(&kitty);
		kitty.compress = cb->f[1][0]-'0';
		iunlock(&kitty);
	}else if(cb->nf == 2 && strcmp(cb->f[0], "batch") == 0){
		batch = strtol(cb->f[1], &ep, 10);
		if(*cb->f[1] == 0 || *ep != 0 || batch < 1 || batch > Maxbatch)
			error(Ebadctl);
		ilock(&kitty);
		kitty.batch = batch;
		iunlock(&kitty);
	}else
		error(Ebadctl);
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
