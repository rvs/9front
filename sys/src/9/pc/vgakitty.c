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
#include "../port/kittyscreen.h"

extern VGAcur vgasoftcur;
extern VGAdev vgakittydev;

static void
vgakittyflush(VGAscr*, Rectangle r)
{
	kittyflush(r);
}

static void
kittydisable(VGAscr*)
{
	error("kitty display cannot be detached");
}

VGAdev vgakittydev = {
	"kitty",
	nil,
	kittydisable,
	nil,
	nil,
	nil,
	nil,
	nil,
	nil,
	vgakittyflush,
};

void
kittyscreeninit(int w, int h)
{
	VGAscr *scr;

	scr = &vgascreen[0];
	deletescreenimage();
	scr->dev = &vgakittydev;
	scr->paddr = 0;
	scr->vaddr = nil;
	scr->softscreen = 1;
	setscreensize(scr, w, h, 32, XRGB32, 0);
	vgascreenwin(scr);
	scr->cur = &vgasoftcur;
	vgasoftcur.enable(scr);
	resetscreenimage();
}

void
kittyscreenresize(int w, int h)
{
	VGAscr *scr;

	scr = &vgascreen[0];
	cursoroff();
	deletescreenimage();
	setscreensize(scr, w, h, 32, XRGB32, 0);
	vgascreenwin(scr);
	vgasoftcur.enable(scr);
	setcursor(&cursor);
	cursoron();
	resetscreenimage();
}

Memimage*
kittyscreenlock(void)
{
	lock(&vgascreenlock);
	return vgascreen[0].gscreen;
}

void
kittyscreenunlock(void)
{
	unlock(&vgascreenlock);
}
