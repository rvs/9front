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

extern void kittyflush(VGAscr*, Rectangle);

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
	kittyflush,
};
