#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

static Socmod *mod;

void
addsocmod(Socmod *s)
{
	if(mod == nil && dtbsocmatch(s->compat)){
		print("soc module: %s\n", s->compat);
		mod = s;
	}
}

/* from archinit, after every module's link */
void
boardinit(void)
{
	if(mod != nil && mod->init != nil)
		mod->init();
	screeninit();
}

void
boardreset(void)
{
	if(mod != nil && mod->reset != nil)
		mod->reset();
}

void
boardpoweroff(void)
{
	if(mod != nil && mod->poweroff != nil)
		mod->poweroff();
}
