/*
 * SpacemiT K1: BPI-F3 and kin
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "k1fns.h"

static void
k1init(void)
{
	k1thermalinit();
}

static Socmod k1mod = {
	"spacemit,k1",
	k1init,
	k1reset,
	k1poweroff,
};

void
k1link(void)
{
	addsocmod(&k1mod);
}
