#include "lib.h"
#include <stdlib.h>
#include "dir.h"

/*
 * A file created in Plan 9 gets only the permissions
 * its directory has; give it all of perm, as POSIX does.
 */
void
_crmode(int fd, int perm)
{
	Dir *d, nd;

	d = _dirfstat(fd);
	if(d == 0)
		return;
	if((d->mode&0777) != perm){
		_nulldir(&nd);
		nd.mode = (d->mode&~0777)|perm;
		_dirfwstat(fd, &nd);
	}
	free(d);
}
