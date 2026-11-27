#include "../git-compat-util.h"

int
statvfs(const char *path, struct statvfs *buf)
{
	errno = ENOSYS;
	return -1;
}

char *
getpass(const char *prompt)
{
	static char buf[256];
	int fd, n;

	fd = open("/dev/cons", O_RDWR);
	if (fd < 0)
		return NULL;
	write(fd, prompt, strlen(prompt));
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n < 0)
		return NULL;
	buf[n] = 0;
	buf[strcspn(buf, "\n")] = 0;
	return buf;
}

int
getpagesize(void)
{
	return 4096;
}

pid_t
getpgid(pid_t pid)
{
	return getpgrp();
}
