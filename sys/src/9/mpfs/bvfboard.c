/*
 * BeagleV-Fire board devices, run from archinit, and their device
 * tree properties: no devices to bring up, and mem.c takes the
 * bank of RAM for user pages from the tree.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

void
boardinit(void)
{
}

void
boarddtbprop(char *path, char *key, uchar *val, int len)
{
	memdtbprop(path, key, val, len);
}
