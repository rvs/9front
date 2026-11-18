/*
 * Microchip PolarFire SoC: the rva23 memory layout, booted by u-boot's
 * booti at 0x80200000 on a U54 hart.  Hart 0 is the E51 monitor core,
 * which only runs M-mode (the HSS), so cpu0 is hart 1.
 */
#include "../rva23/mem.h"

#undef FIRSTHART
#define FIRSTHART	1

#undef TEXTOFF
#define TEXTOFF		0x200000
