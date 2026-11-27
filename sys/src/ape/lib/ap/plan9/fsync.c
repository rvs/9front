#include "lib.h"
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include "dir.h"

/* a wstat that changes nothing asks the file server to commit the file */
int
fsync(int fd)
{
	Dir d;

	_nulldir(&d);
	if(_dirfwstat(fd, &d) < 0){
		_syserrno();
		return -1;
	}
	return 0;
}
