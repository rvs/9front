typedef struct Kitty Kitty;

enum {
	Kittyack,
	Kittykeyboard,
	Kittygraphics,
	Kittypixel,
	Kittyinband,
	Kittyquery = 2147483647,
};

struct Kitty {
	void	*arg;
	void	(*key)(void*, Rune, int);
	void	(*text)(void*, Rune);
	void	(*mouse)(void*, int, int, int, int);
	void	(*resize)(void*, int, int, int, int);
	void	(*reply)(void*, int, int);

	int	pixel;		/* mouse reports use zero-origin pixels */
	int	query;		/* outstanding graphics query identifier */
	int	keyboard;	/* keyboard event reporting has been enabled */
	int	cellw;		/* CSI 6 t cell geometry, before resize callback */
	int	cellh;
	int	rows;
	int	cols;
	int	width;
	int	height;
	int	esc;		/* kittytick expires a pending lone escape */
	int	state;
	int	ncsi;
	char	csi[1024];
	int	nstr;
	char	str[128];
	int	strkind;
	int	overflow;
	int	npaste;
	uchar	utf[UTFmax];
	int	nutf;
	int	alt;
	int	mods;
	int	buttons;
	int	x;
	int	y;
	int	ndown;
	struct {
		int	id;
		Rune	r;
	} down[64];
};

/* one caller owns the parser; callbacks must not reenter it */
void	kittyinit(Kitty*, void*);
void	kittyinput(Kitty*, uchar*, int);
void	kittytick(Kitty*);
void	kittyreset(Kitty*);
