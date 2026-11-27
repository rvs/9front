#include <unistd.h>

int
execlp(const char *name, const char *arg0, ...)
{
	return execvp(name, &arg0);
}
