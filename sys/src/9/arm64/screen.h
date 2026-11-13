/* devmouse.c */
typedef struct Cursor Cursor;
extern Cursor cursor;
extern void	mousetrack(int, int, int, ulong);
extern void	absmousetrack(int, int, int, ulong);
extern Point	mousexy(void);
extern void	mouseaccelerate(int);
extern void	mouseresize(void);
extern void	mouseredraw(void);

/* screen.c */
extern Memimage	*gscreen;
extern Memdata*	attachscreen(Rectangle*, ulong*, int*, int*, int*);
extern void	flushmemscreen(Rectangle);
extern void	blankscreen(int);
extern void	cursoron(void);
extern void	cursoroff(void);
extern void	setcursor(Cursor*);
extern void	mousectl(Cmdbuf*);

/* devdraw.c */
extern QLock	drawlock;
extern void	deletescreenimage(void);
extern void	resetscreenimage(void);

#define ishwimage(i)	(gscreen != nil && (i)->data == gscreen->data)

/* swcursor.c */
extern void	swcursorhide(int);
extern void	swcursoravoid(Rectangle);
extern void	swcursordraw(Point);
extern void	swcursorload(Cursor*);
extern void	swcursorinit(void);
