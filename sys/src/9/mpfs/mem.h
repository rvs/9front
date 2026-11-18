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
