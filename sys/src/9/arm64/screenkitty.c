#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"

#define Image IMAGE
#include <draw.h>
#include <memdraw.h>
#include <cursor.h>
#include "screen.h"
#include "../port/kittyscreen.h"

enum {
	Maxwidth	= 1920,
	Maxheight	= 1200,

	CMaccelerated,
	CMlinear,
	CMres,
};

Memimage *gscreen;

static Lock screenlock;

static Cmdtab mousectlmsg[] = {
	CMaccelerated,	"accelerated",	0,
	CMlinear,	"linear",	1,
	CMres,		"res",		2,
};

static Memimage*
newscreen(int width, int height)
{
	Memimage *i;

	if(width <= 0 || width > Maxwidth || height <= 0 || height > Maxheight)
		error("bad screen size");
	if(memimageinit() < 0)
		error("memimageinit failed");
	i = allocmemimage(Rect(0, 0, width, height), XRGB32);
	if(i == nil)
		error(Enomem);
	memfillcolor(i, DBlack);
	return i;
}

void
kittyscreeninit(int width, int height)
{
	Memimage *i;

	if(gscreen != nil)
		error("screen already initialized");
	i = newscreen(width, height);
	ilock(&screenlock);
	gscreen = i;
	iunlock(&screenlock);
	swcursorinit();
	setcursor(&cursor);
	cursoron();
}

void
kittyscreenresize(int width, int height)
{
	Memimage *i, old;

	if(gscreen == nil)
		error("screen not initialized");
	i = newscreen(width, height);
	cursoroff();
	deletescreenimage();

	/* mouse tracking can read gscreen without drawlock */
	ilock(&screenlock);
	old = *gscreen;
	*gscreen = *i;
	*i = old;
	gscreen->data->imref = gscreen;
	i->data->imref = nil;
	iunlock(&screenlock);
	freememimage(i);

	swcursorinit();
	setcursor(&cursor);
	cursoron();
	resetscreenimage();
	flushmemscreen(gscreen->r);
}

Memimage*
kittyscreenlock(void)
{
	ilock(&screenlock);
	return gscreen;
}

void
kittyscreenunlock(void)
{
	iunlock(&screenlock);
}

Memdata*
attachscreen(Rectangle *r, ulong *chan, int *depth, int *width, int *softscreen)
{
	if(gscreen == nil)
		return nil;
	*r = gscreen->r;
	*chan = gscreen->chan;
	*depth = gscreen->depth;
	*width = gscreen->width;
	*softscreen = 1;
	gscreen->data->ref++;
	return gscreen->data;
}

void
flushmemscreen(Rectangle r)
{
	if(gscreen != nil && rectclip(&r, gscreen->r))
		kittyflush(r);
}

int
hwdraw(Memdrawparam *par)
{
	Memdata *data;

	if(gscreen == nil)
		return 0;
	data = gscreen->data;
	/* drawlock serializes drawing; cursor draws already hold screenlock */
	if(par->dst != nil && par->dst->data == data)
		swcursoravoid(par->r);
	if(par->src != nil && par->src->data == data)
		swcursoravoid(par->sr);
	if(par->mask != nil && par->mask->data == data)
		swcursoravoid(par->mr);
	return 0;
}

void
cursoroff(void)
{
	ilock(&screenlock);
	swcursorhide(1);
	iunlock(&screenlock);
}

void
cursoron(void)
{
	ilock(&screenlock);
	swcursorhide(0);
	swcursordraw(mousexy());
	iunlock(&screenlock);
}

void
setcursor(Cursor *curs)
{
	ilock(&screenlock);
	swcursorload(curs);
	iunlock(&screenlock);
}

void
mousectl(Cmdbuf *cb)
{
	Cmdtab *ct;

	ct = lookupcmd(cb, mousectlmsg, nelem(mousectlmsg));
	switch(ct->index){
	case CMaccelerated:
		mouseaccelerate(cb->nf == 1 ? 1 : atoi(cb->f[1]));
		break;
	case CMlinear:
		mouseaccelerate(0);
		break;
	case CMres:
		break;
	}
}

void
getcolor(ulong p, ulong *r, ulong *g, ulong *b)
{
	ulong rgb;

	rgb = cmap2rgb(p & 255);
	*r = ((rgb >> 16) & 255)*0x01010101;
	*g = ((rgb >> 8) & 255)*0x01010101;
	*b = (rgb & 255)*0x01010101;
}

int
setcolor(ulong p, ulong r, ulong g, ulong b)
{
	USED(p, r, g, b);
	return 0;
}

void
blankscreen(int blank)
{
	USED(blank);
}
