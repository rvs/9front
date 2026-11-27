#include <sys/types.h>
#include <sys/stat.h>
#include "lib.h"

/*
 * No such concept in plan9: remembered, applied by
 * open, creat and mkdir, and passed across exec in $_umask
 */
mode_t	_umask;

mode_t
umask(mode_t m)
{
	mode_t o;

	o = _umask;
	_umask = m & 0777;
	return o;
}
