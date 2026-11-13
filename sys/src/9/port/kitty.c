#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include <keyboard.h>
#include "../port/kitty.h"

enum {
	Ground, Escape, Csi, Ss3, String, Stresc, Paste,
	Press = 1, Repeat, Release,
};

typedef struct Params Params;
struct Params {
	int v[5][32];
	int ns[5];
	int nf;
	int prefix;
	int dollar;
};

static int
scalar(int r)
{
	return r >= 0 && r <= 0x10ffff && (r < 0xd800 || r > 0xdfff);
}

static void
emitkey(Kitty *k, Rune r, int down)
{
	if(r != 0 && k->key != nil)
		k->key(k->arg, r, down);
}

static void
typekey(Kitty *k, Rune r)
{
	emitkey(k, r, 1);
	emitkey(k, r, 0);
}

static int
modbit(Rune r)
{
	switch(r){
	case Kshift: return 1;
	case Kalt: return 2;
	case Kctl: return 4;
	case Kmod4: return 8;
	}
	return 0;
}

static void
modifiers(Kitty *k, int mods)
{
	static Rune keys[] = {Kshift, Kalt, Kctl, Kmod4};
	int i, bit;

	mods &= 15;
	for(i = 0; i < nelem(keys); i++){
		bit = 1<<i;
		if((k->mods ^ mods) & bit)
			emitkey(k, keys[i], (mods & bit) != 0);
	}
	k->mods = mods;
}

void
kittyreset(Kitty *k)
{
	while(k->ndown > 0)
		emitkey(k, k->down[--k->ndown].r, 0);
	modifiers(k, 0);
	if(k->buttons != 0 && k->mouse != nil)
		k->mouse(k->arg, k->x, k->y, 0, k->pixel);
	k->buttons = 0;
	k->state = Ground;
	k->ncsi = k->nstr = k->strkind = k->overflow = 0;
	k->npaste = k->nutf = k->alt = k->esc = 0;
}

void
kittyinit(Kitty *k, void *arg)
{
	memset(k, 0, sizeof *k);
	k->arg = arg;
}

static Rune
codekey(int code)
{
	static Rune basic[] = {
		Kesc, '\n', '\t', Kbs, Kins, Kdel, Kleft, Kright,
		Kup, Kdown, Kpgup, Kpgdown, Khome, Kend,
	};
	static Rune keypad[] = {
		'0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
		'.', '/', '*', '-', '+', '\n', '=', ',', Kleft, Kright,
		Kup, Kdown, Kpgup, Kpgdown, Khome, Kend, Kins, Kdel,
	};

	if(code >= 57344 && code <= 57357)
		return basic[code-57344];
	if(code >= 57364 && code <= 57375)
		return KF | (code-57363);
	if(code >= 57399 && code <= 57426)
		return keypad[code-57399];
	switch(code){
	case 13: return '\n';
	case 127: return Kbs;
	case 57358: return Kcaps;
	case 57359: return Kscroll;
	case 57360: return Knum;
	case 57361: return Kprint;
	case 57362: return Kbreak;
	case 57428: case 57429: case 57430: return Kpause;
	case 57434: return Ksfwd;
	case 57435: return Ksbwd;
	case 57438: return Kvoldn;
	case 57439: return Kvolup;
	case 57440: return Kmute;
	case 57441: case 57447: return Kshift;
	case 57442: case 57448: return Kctl;
	case 57443: case 57449: return Kalt;
	case 57444: case 57450: return Kmod4;
	case 57453: return Kaltgr;
	}
	/* reserve the protocol's functional-key range */
	if((code >= 0xe000 && code <= 0xf8ff) || !scalar(code))
		return 0;
	return code;
}

static Rune
letterkey(int c)
{
	switch(c){
	case 'A': return Kup;
	case 'B': return Kdown;
	case 'C': return Kright;
	case 'D': return Kleft;
	case 'H': return Khome;
	case 'F': return Kend;
	case 'P': return KF|1;
	case 'Q': return KF|2;
	case 'R': return KF|3;
	case 'S': return KF|4;
	}
	return 0;
}

static Rune
tildekey(int n)
{
	static int fn[] = {11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 23, 24};
	int i;

	for(i = 0; i < nelem(fn); i++)
		if(n == fn[i])
			return KF|(i+1);
	switch(n){
	case 1: case 7: return Khome;
	case 2: return Kins;
	case 3: return Kdel;
	case 4: case 8: return Kend;
	case 5: return Kpgup;
	case 6: return Kpgdown;
	}
	return 0;
}

static void
keyevent(Kitty *k, int id, Rune r, int ev)
{
	int i;

	for(i = 0; i < k->ndown; i++)
		if(k->down[i].id == id)
			break;
	if(ev == Release){
		if(i < k->ndown){
			emitkey(k, k->down[i].r, 0);
			k->down[i] = k->down[--k->ndown];
		}
		return;
	}
	if(r == 0)
		return;
	if(i == k->ndown){
		/* never leave an untracked key held */
		if(i == nelem(k->down)){
			typekey(k, r);
			return;
		}
		k->ndown++;
	}else if(k->down[i].r != r)
		emitkey(k, k->down[i].r, 0);
	k->down[i].id = id;
	k->down[i].r = r;
	emitkey(k, r, 1);
}

static int
params(char *s, int n, Params *p)
{
	int i, f, a, c, v;

	memset(p, 0, sizeof *p);
	memset(p->v, 0xff, sizeof p->v);
	f = a = 0;
	p->ns[0] = 1;
	if(n > 0 && (*s == '?' || *s == '<')){
		p->prefix = *s++;
		n--;
	}
	if(n > 0 && s[n-1] == '$'){
		p->dollar = 1;
		n--;
	}
	for(i = 0; i < n; i++){
		c = s[i];
		if(c >= '0' && c <= '9'){
			v = p->v[f][a];
			if(v < 0)
				v = 0;
			if(v > (0x7fffffff-(c-'0'))/10)
				return 0;
			p->v[f][a] = v*10+(c-'0');
		}else if(c == ':'){
			if(++a == nelem(p->v[0]))
				return 0;
			p->ns[f]++;
		}else if(c == ';'){
			if(++f == nelem(p->v))
				return 0;
			a = 0;
			p->ns[f] = 1;
		}else
			return 0;
	}
	p->nf = f+1;
	return 1;
}

static int
field(Params *p, int f, int a, int def)
{
	if(f >= p->nf || a >= p->ns[f] || p->v[f][a] < 0)
		return def;
	return p->v[f][a];
}

static void
reply(Kitty *k, int kind, int value)
{
	if(k->reply != nil)
		k->reply(k->arg, kind, value);
}

static void
keyboard(Kitty *k, Params *p, int final)
{
	int code, mods, ev, id, i, shifted, bit;
	Rune r;

	if(p->nf > 3 || p->ns[0] > 3 || p->ns[1] > 2)
		return;
	code = field(p, 0, 0, 0);
	mods = field(p, 1, 0, 1);
	ev = field(p, 1, 1, Press);
	if(mods < 1 || mods > 256 || ev < Press || ev > Release)
		return;
	for(i = 0; i < p->ns[0]; i++)
		if(!scalar(field(p, 0, i, 0)))
			return;
	if(p->nf == 3)
		for(i = 0; i < p->ns[2]; i++)
			if(!scalar(field(p, 2, i, -1)) || p->v[2][i] < 0x20 ||
			   (p->v[2][i] >= 0x7f && p->v[2][i] <= 0x9f))
				return;
	mods--;
	if(final == 'u'){
		r = codekey(code);
		id = code;
	}else if(final == '~'){
		r = tildekey(code);
		id = 0x110000+code;
	}else{
		r = letterkey(final);
		id = 0x120000+final;
	}
	bit = modbit(r);
	modifiers(k, mods);
	if(bit)
		return;
	if(ev == Release){
		keyevent(k, id, r, ev);
		return;
	}
	if(final == 'u' && p->nf == 3){
		if(k->text != nil)
			for(i = 0; i < p->ns[2]; i++)
				k->text(k->arg, p->v[2][i]);
		return;
	}
	shifted = field(p, 0, 1, 0);
	if(final == 'u' && (mods & 1) && shifted > 0)
		r = codekey(shifted);
	if((mods & 4) && r >= 0x20 && r < 0x7f)
		r &= 0x1f;
	/* legacy CSI sequences without event types are complete keystrokes */
	if(!k->keyboard && p->ns[1] < 2 && final != 'u'){
		typekey(k, r);
		modifiers(k, 0);
	}else
		keyevent(k, id, r, ev);
}

static void
mouse(Kitty *k, Params *p, int release)
{
	int b, x, y, bit;

	if(p->nf != 3 || p->ns[0] != 1 || p->ns[1] != 1 || p->ns[2] != 1)
		return;
	b = p->v[0][0];
	x = p->v[1][0];
	y = p->v[2][0];
	if(b < 0 || b > 127 || x < 0 || y < 0 || (!k->pixel && (x == 0 || y == 0)))
		return;
	k->x = x;
	k->y = y;
	if(b & 64){
		if(release || (b & 32) || (b & 3) > 1)
			return;
		bit = (b & 1) ? 16 : 8;
		if(k->mouse != nil){
			k->mouse(k->arg, x, y, k->buttons|bit, k->pixel);
			k->mouse(k->arg, x, y, k->buttons, k->pixel);
		}
		return;
	}
	if(!(b & 32) && (b & 3) != 3){
		bit = 1<<(b & 3);
		if(release)
			k->buttons &= ~bit;
		else
			k->buttons |= bit;
	}
	if(k->mouse != nil)
		k->mouse(k->arg, x, y, k->buttons, k->pixel);
}

static void
csi(Kitty *k, int final)
{
	Params p;
	int a, b, c, d, i;

	if(!params(k->csi, k->ncsi, &p))
		return;
	if(p.prefix == '<'){
		if(!p.dollar && (final == 'M' || final == 'm'))
			mouse(k, &p, final == 'm');
		return;
	}
	if(p.prefix == '?'){
		if(final == 'u' && !p.dollar && p.nf == 1 && p.ns[0] == 1 && p.v[0][0] >= 0)
			reply(k, Kittykeyboard, p.v[0][0]);
		if(final == 'y' && p.dollar && p.nf == 2 && p.ns[0] == 1 && p.ns[1] == 1){
			a = p.v[0][0];
			b = p.v[1][0];
			if(a == 1016 || a == 2048)
				reply(k, a == 1016 ? Kittypixel : Kittyinband, b == 1 || b == 2 || b == 3);
		}
		return;
	}
	if(p.dollar)
		return;
	if(final == 'n' && p.nf == 1 && p.ns[0] == 1 && p.v[0][0] == 0)
		reply(k, Kittyack, 1);
	else if(final == 'O' && k->ncsi == 0)
		kittyreset(k);
	else if(final == '~' && p.nf == 1 && p.ns[0] == 1 && p.v[0][0] == 200){
		k->state = Paste;
		k->npaste = k->nutf = 0;
	}else if(final == 'u' || final == '~' || letterkey(final) != 0)
		keyboard(k, &p, final);
	else if(final == 't'){
		for(i = 0; i < p.nf; i++)
			if(p.ns[i] != 1 || p.v[i][0] <= 0)
				return;
		a = field(&p, 1, 0, 0);
		b = field(&p, 2, 0, 0);
		c = field(&p, 3, 0, 0);
		d = field(&p, 4, 0, 0);
		switch(p.v[0][0]){
		case 4:
			if(p.nf != 3)
				break;
			k->width = b;
			k->height = a;
			if(k->resize != nil)
				k->resize(k->arg, 0, 0, b, a);
			break;
		case 6:
			if(p.nf != 3)
				break;
			k->cellh = a;
			k->cellw = b;
			if(k->resize != nil)
				k->resize(k->arg, 0, 0, 0, 0);
			break;
		case 8:
			if(p.nf != 3)
				break;
			k->rows = a;
			k->cols = b;
			if(k->resize != nil)
				k->resize(k->arg, a, b, 0, 0);
			break;
		case 48:
			if(p.nf != 5)
				break;
			k->rows = a;
			k->cols = b;
			k->width = d;
			k->height = c;
			if(k->resize != nil)
				k->resize(k->arg, a, b, d, c);
			break;
		}
	}
}

static void
string(Kitty *k)
{
	char *s, *e;
	int id, v;

	if(k->overflow || k->strkind != '_' || k->nstr < 7 || k->str[0] != 'G')
		return;
	k->str[k->nstr] = 0;
	s = k->str+1;
	id = 0;
	while(*s != 0 && *s != ';'){
		e = s;
		while(*e != 0 && *e != ',' && *e != ';')
			e++;
		if(s[0] == 'i' && s[1] == '='){
			if(id != 0)
				return;
			s += 2;
			v = 0;
			if(s == e)
				return;
			for(; s < e; s++){
				if(*s < '0' || *s > '9' || v > (0x7fffffff-(*s-'0'))/10)
					return;
				v = v*10+(*s-'0');
			}
			id = v;
		}
		s = e;
		if(*s == ',')
			s++;
	}
	if(id == Kittyquery && *s == ';')
		reply(k, Kittygraphics, strcmp(s+1, "OK") == 0);
}

static void
rune(Kitty *k, Rune r, int paste)
{
	if(paste){
		if(r == '\r')
			r = '\n';
		if((r >= 0x20 && (r < 0x7f || r > 0x9f)) || r == '\n' || r == '\t')
			if(k->text != nil)
				k->text(k->arg, r);
	}else{
		if(r == '\r')
			r = '\n';
		else if(r == 0x7f)
			r = Kbs;
		if(k->alt)
			emitkey(k, Kalt, 1);
		typekey(k, r);
		if(k->alt)
			emitkey(k, Kalt, 0);
		k->alt = 0;
	}
}

static void
utfbyte(Kitty *k, int c, int paste)
{
	int n, i, min;
	Rune r;

	if(k->nutf > 0 && (c & 0xc0) != 0x80){
		k->nutf = 0;
		rune(k, Runeerror, paste);
	}
	if(k->nutf == 0 && c < 0x80){
		rune(k, c, paste);
		return;
	}
	if(k->nutf == 0 && (c < 0xc2 || c > 0xf4)){
		rune(k, Runeerror, paste);
		return;
	}
	k->utf[k->nutf++] = c;
	c = k->utf[0];
	n = c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
	if(k->nutf < n)
		return;
	min = n == 2 ? 0x80 : n == 3 ? 0x800 : 0x10000;
	r = c & ((1<<(7-n))-1);
	for(i = 1; i < n; i++)
		r = (r<<6) | (k->utf[i] & 0x3f);
	k->nutf = 0;
	if(r < min || !scalar(r))
		r = Runeerror;
	rune(k, r, paste);
}

void
kittytick(Kitty *k)
{
	if(k->esc){
		k->esc = 0;
		k->state = Ground;
		typekey(k, Kesc);
	}
}

void
kittyinput(Kitty *k, uchar *s, int n)
{
	static char endpaste[] = "\033[201~";
	int c, i;

	while(n-- > 0){
		c = *s++;
		switch(k->state){
		case Ground:
			if(c == 0x1b){
				if(k->nutf != 0){
					k->nutf = 0;
					rune(k, Runeerror, 0);
				}
				k->state = Escape;
				k->esc = 1;
			}else
				utfbyte(k, c, 0);
			break;
		case Escape:
			k->esc = 0;
			k->state = Ground;
			if(c == '['){
				k->state = Csi;
				k->ncsi = k->overflow = 0;
			}else if(c == 'O')
				k->state = Ss3;
			else if(c == ']' || c == '_' || c == 'P' || c == '^' || c == 'X'){
				k->state = String;
				k->strkind = c;
				k->nstr = k->overflow = 0;
			}else if(c == 0x1b){
				typekey(k, Kesc);
				k->state = Escape;
				k->esc = 1;
			}else{
				k->alt = 1;
				utfbyte(k, c, 0);
			}
			break;
		case Ss3:
			k->state = Ground;
			typekey(k, letterkey(c));
			break;
		case Csi:
			if(c >= 0x40 && c <= 0x7e){
				k->state = Ground;
				if(!k->overflow)
					csi(k, c);
			}else if(k->ncsi < sizeof k->csi)
				k->csi[k->ncsi++] = c;
			else
				k->overflow = 1;
			break;
		case String:
			if(c == 7){
				string(k);
				k->state = Ground;
			}else if(c == 0x1b)
				k->state = Stresc;
			else if(k->nstr < sizeof k->str-1)
				k->str[k->nstr++] = c;
			else
				k->overflow = 1;
			break;
		case Stresc:
			if(c == '\\' || c == 7){
				string(k);
				k->state = Ground;
			}else{
				k->overflow = 1;
				k->state = c == 0x1b ? Stresc : String;
			}
			break;
		case Paste:
			if(c == endpaste[k->npaste]){
				if(++k->npaste == sizeof endpaste-1){
					if(k->nutf != 0)
						rune(k, Runeerror, 1);
					k->state = Ground;
					k->npaste = k->nutf = 0;
				}
			}else{
				for(i = 0; i < k->npaste; i++)
					utfbyte(k, endpaste[i], 1);
				k->npaste = 0;
				if(c == endpaste[0])
					k->npaste = 1;
				else
					utfbyte(k, c, 1);
			}
			break;
		}
	}
}
