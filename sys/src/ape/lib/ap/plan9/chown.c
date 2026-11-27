#include "lib.h"
#include "sys9.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include "dir.h"

int
chown(const char *path, uid_t owner, gid_t group)
{
	int num;
	Dir d;
	struct stat st;

	_nulldir(&d);

	/* -1 or the current value means no change */
	if(stat(path, &st) < 0)
		return -1;
	if(owner == st.st_uid)
		owner = -1;
	if(group == st.st_gid)
		group = -1;

	/* find owner, group */
	if(owner != -1){
		d.uid = nil;
		num = owner;
		if(!_getpw(&num, &d.uid, 0)) {
			errno = EINVAL;
			return -1;
		}
	}
	if(group != -1){
		d.gid = nil;
		num = group;
		if(!_getpw(&num, &d.gid, 0)) {
			errno = EINVAL;
			return -1;
		}
	}
	if(owner == -1 && group == -1)
		return 0;

	if(_dirwstat(path, &d) < 0){
		_syserrno();
		return -1;
	}
	return 0;
}
