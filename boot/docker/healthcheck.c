/* Probe the real 9P worker, not merely the TCP listener. Linux/POSIX, no deps. */
#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int transfer(int fd, unsigned char *p, size_t n, int writing)
{
	while (n) {
		ssize_t k = writing ? send(fd, p, n, MSG_NOSIGNAL) : recv(fd, p, n, 0);
		if (k < 0 && errno == EINTR) continue;
		if (k <= 0) return -1;
		p += k;
		n -= (size_t)k;
	}
	return 0;
}

static int rpc(int fd, unsigned char *request, size_t n, int response)
{
	unsigned char b[8192];
	uint32_t size;
	if (transfer(fd, request, n, 1) || transfer(fd, b, 4, 0)) return -1;
	size = (uint32_t)b[0] | (uint32_t)b[1]<<8 | (uint32_t)b[2]<<16 | (uint32_t)b[3]<<24;
	if (size < 7 || size > sizeof b || transfer(fd, b+4, size-4, 0)) return -1;
	if (b[4] != response || b[5] != request[5] || b[6] != request[6]) return -1;
	if (response == 101 && (size != 19 || b[11] != 6 || b[12] || memcmp(b+13, "9P2000", 6))) return -1;
	if (response == 105 && size != 20) return -1;
	if (response == 125 && size < 50) return -1;
	return 0;
}

int main(void)
{
	struct sockaddr_in addr = {.sin_family=AF_INET, .sin_port=htons(564)};
	struct timeval timeout = {.tv_sec=3};
	/* msize=8192, NOTAG; attach fid1 as glenda, NOFID, empty aname; stat fid1. */
	unsigned char version[] = {19,0,0,0,100,255,255,0,32,0,0,6,0,'9','P','2','0','0','0'};
	unsigned char attach[] = {25,0,0,0,104,1,0,1,0,0,0,255,255,255,255,6,0,'g','l','e','n','d','a',0,0};
	unsigned char status[] = {11,0,0,0,124,2,0,1,0,0,0};
	attach[0] = sizeof attach;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return 1;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
	int failed = connect(fd, (struct sockaddr *)&addr, sizeof addr) ||
		rpc(fd, version, sizeof version, 101) ||
		rpc(fd, attach, sizeof attach, 105) ||
		rpc(fd, status, sizeof status, 125);
	close(fd);
	if (failed) fputs("9P version/attach/stat probe failed\n", stderr);
	return failed ? 1 : 0;
}
