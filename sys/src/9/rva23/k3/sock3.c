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

Socmod sock3 = {
	"k3",
	"spacemit,k3",
	k3init,
};
