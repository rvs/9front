/* no statvfs on Plan 9; callers treat failure as unknown */
struct statvfs {
	unsigned long f_bsize;
	unsigned long f_frsize;
	unsigned long f_flag;
	unsigned long long f_blocks;
	unsigned long long f_bfree;
	unsigned long long f_bavail;
};
int statvfs(const char *, struct statvfs *);
