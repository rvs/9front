#include <unistd.h>
#include <stdlib.h>
#include <sys/limits.h>
#include <string.h>
#include <errno.h>

extern char **environ;

/*
 * Search $PATH for name.  Without PATH, try name
 * itself and then /bin/name.
 */
int
execvp(const char *name, const char **argv)
{
	char buf[PATH_MAX], *path, *p, *e;
	int n, l, err;

	if(strchr(name, '/') != 0)
		return execve(name, argv, environ);
	path = getenv("PATH");
	if(path == 0){
		if((n=execve(name, argv, environ)) < 0){
			if(strlen(name) >= sizeof(buf)-5)
				return n;
			strcpy(buf, "/bin/");
			strcpy(buf+5, name);
			n = execve(buf, argv, environ);
		}
		return n;
	}
	err = ENOENT;
	l = strlen(name);
	for(p = path; ; p = e+1){
		e = strchr(p, ':');
		if(e == 0)
			e = p+strlen(p);
		if(e == p){
			buf[0] = '.';
			n = 1;
		}else{
			n = e-p;
			memmove(buf, p, n);
		}
		if(n+1+l < sizeof(buf)){
			buf[n] = '/';
			strcpy(buf+n+1, name);
			execve(buf, argv, environ);
			if(errno != ENOENT)
				err = errno;
		}
		if(*e == 0)
			break;
	}
	errno = err;
	return -1;
}
