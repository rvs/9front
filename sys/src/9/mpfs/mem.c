/*
 * PolarFire SoC memory: the kernel's bank, the DTB's first, in the
 * DDR the HSS maps cached at 0x80000000, and a bank of user pages in
 * the DDR's cached alias at 0x1000000000, above UZERO, out of the
 * identity map's reach.  The kernel sees the user bank UMEMOFF lower,
 * in a window that 1 GiB leaves in the root table map just below
 * UZERO, and its identity map ends where the window begins.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

extern u64int *sv57, *sv48, *sv39, *pGiB;
extern uintptr klimit;

static uintptr userbase, userlimit;

/* the DTB's first bank of RAM at or above UZERO is for user pages */
void
memdtbprop(char *path, char *key, uchar *val, int len)
{
	if(strncmp(path, "/memory", 7) != 0 || strcmp(key, "reg") != 0)
		return;
	if(len != 16 || userlimit != 0 || nhgetv(val) < UZERO)
		return;
	userbase = nhgetv(val);
	userlimit = userbase + nhgetv(val+8);
}

static void
usermeminit(void)
{
	uintptr pa;

	if(userlimit == 0)
		return;
	if(userlimit > UZERO+UMEMOFF || userbase >= userlimit){
		print("meminit: user ram %#p-%#p outside the window\n",
			userbase, userlimit);
		return;
	}
	conf.mem[1].base = PGROUND(userbase);
	conf.mem[1].limit = userlimit & ~(uintptr)(BY2PG-1);
	conf.mem[1].npage = (conf.mem[1].limit - conf.mem[1].base)/BY2PG;
	for(pa = userbase & ~((uintptr)GiB-1); pa < userlimit; pa += GiB)
		sv39[PTLX(pa-UMEMOFF, 2)] = PA2PTE(pa) | PTEVALID | PTEREAD
			| PTEWRITE | PTEACCESSED | PTEDIRTY;
	print("meminit: user ram %#p-%#p\n", conf.mem[1].base, conf.mem[1].limit);
}

void
meminit(void)
{
	char *p;
	uintptr l;

	klimit = UZERO - UMEMOFF;
	l = VDRAM + 2ULL*GiB;
	if(p = getconf("*maxmem"))
		l = strtoull(p, 0, 0);
	if(l > KLIMIT)
		l = KLIMIT;

	/* after the image, room for the boot page tables */
	conf.mem[0].base = ROUND(PGROUND((uintptr)end - KTZERO)
		+ (uintptr)(VDRAM+0x2fffff), 0x200000);
	conf.mem[0].limit = l;
	if(l <= conf.mem[0].base)
		panic("meminit: no ram above the kernel");
	sv57 = (void*)PGROUND((uintptr)end);
	sv48 = (void*)PGROUND((uintptr)sv57 + 1);
	sv39 = (void*)PGROUND((uintptr)sv48 + 1);
	pGiB = (void*)PGROUND((uintptr)sv39 + 1);
	memset(sv57, 0, 16384);
	usermeminit();
	kmapram(conf.mem[0].base, l);

	conf.mem[0].npage = (conf.mem[0].limit - conf.mem[0].base)/BY2PG;
}

void*
kmapaddr(uintptr pa)
{
	if(pa >= conf.mem[1].base && pa < conf.mem[1].limit)
		return (void*)(pa - UMEMOFF);
	return kaddr(pa);
}

static uintptr ncnext = NCDMABASE;

/*
 * Zeroed memory from the non-cached DMA window, for device rings
 * and bounce buffers; never freed.
 */
void*
ncalloc(usize n, usize align)
{
	uintptr p;

	p = ROUND(ncnext, align);
	if(p + n > NCDMABASE + NCDMASIZE)
		panic("ncalloc: out of non-cached memory");
	ncnext = p + n;
	memset((void*)p, 0, n);
	return (void*)p;
}
