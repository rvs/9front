/*
 * SpacemiT K3: Pico-ITX and the CoM260 IFX carrier
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "k3fns.h"

static void
k3init(void)
{
	gpiok3init();
	k3thermalinit();
	k3ecfaninit();
	/* each checks for its own carrier board */
	dpuk3();
	dpuk3com260();
}

static Socmod k3mod = {
	"spacemit,k3",
	k3init,
};

void
k3link(void)
{
	addsocmod(&k3mod);
}
