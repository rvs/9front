extern QLock drawlock;

void absmousetrack(int, int, int, ulong);
void kittyflush(Rectangle);

/* called with drawlock held; snapshots also take the screen lock */
void kittyscreeninit(int, int);
void kittyscreenresize(int, int);
Memimage* kittyscreenlock(void);
void kittyscreenunlock(void);
