#include "lib.h"
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include "sys9.h"
#include "dir.h"

/* stat the directory holding the last element e of path */
static Dir*
statparent(const char *path, char *e)
{
	char buf[8192];
	int n;

	n = e-path;
	if(n == 0)
		return _dirstat(".");
	if(n == 1)
		return _dirstat("/");
	if(n-1 >= sizeof(buf))
		return nil;
	memmove(buf, path, n-1);
	buf[n-1] = 0;
	return _dirstat(buf);
}

static int
samedir(const char *from, char *f, const char *to, char *t)
{
	Dir *a, *b;
	int r;

	if(f-from==t-to && strncmp(from, to, f-from)==0)
		return 1;
	a = statparent(from, f);
	b = statparent(to, t);
	r = a != nil && b != nil
		&& a->qid.path == b->qid.path
		&& a->type == b->type
		&& a->dev == b->dev;
	free(a);
	free(b);
	return r;
}

/* move a directory elsewhere by moving its contents into a new one */
static int
movedir(const char *from, const char *to, Dir *s, Dir *d)
{
	Dir *db;
	char *fe, *te;
	long i, n;
	int fd, r;

	if(d != nil && _REMOVE(to) < 0){
		_syserrno();
		return -1;
	}
	if((fd = _CREATE(to, OREAD, DMDIR|(s->mode&0777))) < 0){
		_syserrno();
		return -1;
	}
	_CLOSE(fd);
	if((fd = _OPEN(from, OREAD)) < 0){
		_syserrno();
		return -1;
	}
	n = _dirreadall(fd, &db);
	_CLOSE(fd);
	r = 0;
	for(i = 0; i < n && r == 0; i++){
		fe = malloc(strlen(from)+strlen(db[i].name)+2);
		te = malloc(strlen(to)+strlen(db[i].name)+2);
		if(fe == nil || te == nil){
			errno = ENOMEM;
			r = -1;
		}else{
			sprintf(fe, "%s/%s", from, db[i].name);
			sprintf(te, "%s/%s", to, db[i].name);
			r = rename(fe, te);
		}
		free(fe);
		free(te);
	}
	if(n > 0)
		free(db);
	if(r == 0 && _REMOVE(from) < 0){
		_syserrno();
		r = -1;
	}
	return r;
}

int
rename(const char *from, const char *to)
{
	char buf[8192], *f, *t;
	Dir *s, *d, nd;
	int n, ffd, tfd;

	f = strrchr(from, '/');
	t = strrchr(to, '/');
	f = f != nil ? f+1 : (char*)from;
	t = t != nil ? t+1 : (char*)to;

	if(*f == '\0' || strcmp(f, ".") == 0 || strcmp(f, "..") == 0
	|| *t == '\0' || strcmp(t, ".") == 0 || strcmp(t, "..") == 0){
		errno = EINVAL;
		return -1;
	}

	if((s = _dirstat(from)) == nil){
		_syserrno();
		return -1;
	}
	if((d = _dirstat(to)) != nil){
		if(d->qid.type == s->qid.type
		&& d->qid.path == s->qid.path
		&& d->qid.vers == s->qid.vers
		&& d->type == s->type
		&& d->dev == s->dev)
			goto out;	/* same file */

		if((d->mode ^ s->mode) & DMDIR){
			errno = (d->mode & DMDIR) ? EISDIR : ENOTDIR;
			goto err;
		}
	}

	/* from and to are in the same directory: just rename */
	if(samedir(from, f, to, t)){
		if(d != nil && _REMOVE(to) < 0){
			_syserrno();
			goto err;
		}
		_nulldir(&nd);
		nd.name = t;
		if(_dirwstat(from, &nd) < 0){
			_syserrno();
			goto err;
		}
		goto out;
	}

	/* different directories: have to copy */
	if(s->mode & DMDIR){
		if(movedir(from, to, s, d) < 0)
			goto err;
		goto out;
	}
	if((ffd = _OPEN(from, OREAD)) < 0){
		_syserrno();
		goto err;
	}
	if(d != nil && _REMOVE(to) < 0){
		_syserrno();
		_CLOSE(ffd);
		goto err;
	}
	if((tfd = _CREATE(to, OWRITE, s->mode)) < 0){
		_syserrno();
		_CLOSE(ffd);
		goto err;
	}
	while((n = _READ(ffd, buf, sizeof(buf))) > 0){
		if(_WRITE(tfd, buf, n) != n)
			break;
	}
	_CLOSE(ffd);
	_CLOSE(tfd);
	if(n != 0 || _REMOVE(from) < 0){
		_syserrno();
		_REMOVE(to);	/* cleanup */
		goto err;
	}
out:
	free(s);
	free(d);
	return 0;
err:
	free(s);
	free(d);
	return -1;
}
