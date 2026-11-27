/*
 * pANS stdio -- remove
 */
#include "iolib.h"
#include <errno.h>
int remove(const char *f){
	if(unlink((char *)f) == 0)
		return 0;
	if(errno != EPERM)
		return -1;
	return rmdir((char *)f);	/* a directory */
}
