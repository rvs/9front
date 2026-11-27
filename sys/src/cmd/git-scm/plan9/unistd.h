/* APE declares most of unistd.h only after sys/types.h */
#include <sys/types.h>
#include "/sys/include/ape/unistd.h"
extern int fsync(int);
extern int getpagesize(void);
extern pid_t getpgid(pid_t);
