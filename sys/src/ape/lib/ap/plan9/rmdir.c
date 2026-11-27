#include "lib.h"
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include "sys9.h"
#include "dir.h"

int
rmdir(const char *path)
{
	Dir *d;

	/* remove(2) would take a file as well */
	if((d = _dirstat(path)) == nil){
		_syserrno();
		return -1;
	}
	if((d->mode & 0x80000000) == 0){
		free(d);
		errno = ENOTDIR;
		return -1;
	}
	free(d);
	if(_REMOVE(path) < 0){
		_syserrno();
		return -1;
	}
	return 0;
}
