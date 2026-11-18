/*
 * Microchip PolarFire SoC: the rva23 memory layout on the U54 harts.
 * Hart 0 is the E51 monitor core, which only runs M-mode (the HSS), so
 * cpu0 is hart 1, whose S-mode PLIC context is 2: the E51 has an M
 * context only.
 */
#include "../rva23/mem.h"

#undef FIRSTHART
#define FIRSTHART	1

#undef PLICSCTXT
#define PLICSCTXT	2

/*
 * DDR the HSS maps non-cached at 0xc0000000, outside the RAM bank:
 * the DTB's non-cached-low-buffer.  The U54s have no cache-block
 * operations, so device DMA goes through here.
 */
#define NCDMABASE	0xc4000000ULL
#define NCDMASIZE	(64*MiB)
