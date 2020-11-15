#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <mouse.h>
#include <cursor.h>
#include <keyboard.h>

#define DHEIGHT 32
#define ZOOMOUT 512

typedef struct Fetchctl Fetchctl;
struct Fetchctl {
	long state;
	Channel *c;
	Channel *plchan;
	Channel *pos;
};

s8int *pcm, *pp;
u8int *mono8;
long pcmlen, scroll, curpos;
Image *Ibg, *Itrbg, *Itrfg, *Icur, *Irow;
Rectangle rbars;
int dwidth;
int yieldcounter;
long playing;

void usage(void);
void clear(void);
void setdwidth(void);
void drawpcm(Point, ulong);
void drawcur(Point);
void drawcurabs(void);
void clearcurabs(void);
void redraw(ulong);
void resize(void);
void loadpcm(char*);
u8int* mkmono8(s8int*, long);
Point getcurxy(void);
void threadplay(void*);
void threadfetch(void*);
Fetchctl* initfetch(void);
int fillrow(ulong, ulong);
void drawscroll(int);

void
threadmain(int argc, char **argv)
{
	Mousectl *mctl;
	Keyboardctl *kctl;
	Fetchctl *fctl;
	Mouse mv;
	Rune  kv;
	int   rv[2];
	long pv;

	ARGBEGIN{
	default:
		usage();
	}ARGEND;

	if (argc == 0) usage();

	loadpcm(argv[0]);
	mono8 = mkmono8(pcm, pcmlen);

	if (argc <= 0) usage();
	if(initdraw(0, 0, "amp") < 0)
		sysfatal("inidraw failed: %r");
	if((mctl = initmouse(0, screen)) == nil)
		sysfatal("initmouse failed: %r");
	if((kctl = initkeyboard(0)) == nil)
		sysfatal("initkeyboard failed: %r");
	if((fctl = initfetch()) == nil)
		sysfatal("initfetch failed: %r");

	Ibg = allocimage(display, Rect(0,0,1,1), RGB24, 1, 0xBBBBBBFF);
	Itrbg = allocimage(display, Rect(0,0,1,1), RGB24, 1, DWhite);
	Itrfg = allocimage(display, Rect(0,0,1,1), RGB24, 1, DBlack);
	Icur = allocimage(display, Rect(0,0,1,1), RGB24, 1, DRed);

	curpos = 0;
	playing = 0;

	resize();
	Irow = allocimage(display, Rect(0, 0, dwidth, DHEIGHT),
		CMAP8, 0, DBlue);
	clear();
	redraw(0);
	flushimage(display, 1);
	Alt alts[5] = {
		{kctl->c, &kv, CHANRCV},
		{mctl->c, &mv, CHANRCV},
		{mctl->resizec, rv, CHANRCV},
		{fctl->pos, &pv, CHANRCV},
		{0, 0, CHANEND},
	};
	for (;;) {
		switch (alt(alts)) {
		case 0: /* keyboard */
			if (kv == 0x7f) threadexitsall(nil);
			break;
		case 1: /* mouse */
			if (mv.buttons == 0) {
				nbsend(fctl->c, &playing);
			}
			if (mv.buttons == 1) {
				nbsendul(fctl->c, 0);
				clearcurabs();
				long mrow = (mv.xy.y - screen->r.min.y) / (DHEIGHT + 4);
				long mx = mv.xy.x - screen->r.min.x - 4;
				if (mx < 0) mx = 0;
				if (mx >= dwidth) mx = dwidth - 1;
				long newpos = (scroll + mrow) * dwidth + mx;
				curpos = newpos;
				if (curpos >= pcmlen / (ZOOMOUT * 4))
					curpos = pcmlen / (ZOOMOUT * 4) - 1;
				pp = pcm + curpos * (ZOOMOUT * 4);
				drawcurabs();
				flushimage(display, 1);
			}
			if (mv.buttons == 4) {
				playing = (playing)? 0 : 1;
				nbsend(fctl->c, &playing);
			}
			if (mv.buttons == 8) { /* scroll up */
				drawscroll(-1);
				flushimage(display, 1);
			}
			if (mv.buttons == 16) { /* scroll down */
				drawscroll(1);
				flushimage(display, 1);
			}
			break;
		case 2: /* resize */
			if(getwindow(display, Refnone) < 0)
				sysfatal("resize failed: %r");
			resize();
			freeimage(Irow);
			Irow = allocimage(display, Rect(0, 0, dwidth, DHEIGHT),
				CMAP8, 0, DBlue);
			clear();
			redraw(scroll * dwidth);
			flushimage(display, 1);
			break;
		case 3: /* position change */
			clearcurabs();
			curpos = pv / (4 * ZOOMOUT);
			drawcurabs();
			flushimage(display, 1);
			break;
		}
	}
}


void
resize(void)
{
	int height;
	setdwidth();
	rbars = screen->r;
	rbars.min.x += 4;
	rbars.max.x -= 4;
	height = Dy(screen->r) / (DHEIGHT + 4) * (DHEIGHT + 4);
	rbars.min.y += 2 + (Dy(screen->r) - height) / 2;
	rbars.max.y = rbars.min.y + height;

	if (scroll > pcmlen/(4 * ZOOMOUT*dwidth))
		scroll = pcmlen/(4 * ZOOMOUT*dwidth);
}


void
threadplay(void *v)
{
	Channel *c;
	s8int buf[1024];
	int devaudio;
	c = v;
	devaudio = open("/dev/audio", OWRITE);
	if (devaudio <= 0){
		fprint(2, "can't open /dev/audio\n");
		return;
	}
	for (;;) {
		recv(c, buf);
		write(devaudio, buf, 1024);
	}
}


void
threadfetch(void* v)
{
	long pos, len, newstate;
	s8int buf[1024];
	Fetchctl *fctl;
	fctl = v;
	for (;;) {
		recv(fctl->c, &newstate);
		fctl->state = newstate;
		while (fctl->state != 0) {
			if (nbrecv(fctl->c, &newstate) > 0)	fctl->state = newstate;
			memset(buf, 0, 1024);
			len = (pp + 1024 < pcm + pcmlen) ? 1024 : pcm + pcmlen - pp;
			memcpy(buf, pp, len);
			pp += len;
			if (pp >= pcm+pcmlen) {
				pp = pcm;
				fctl->state = 0;
				playing = 0;
			}
			pos = pp-pcm;
			nbsend(fctl->pos, &pos);
			send(fctl->plchan, buf);
		}
	}
}


Fetchctl*
initfetch(void)
{
	Fetchctl *fctl;
	fctl = malloc(sizeof(Fetchctl));
	fctl->state = 0;
	fctl->plchan = chancreate(1024, 0);
	fctl->pos = chancreate(sizeof(long), 0);
	fctl->c = chancreate(sizeof(long), 16);
	threadcreate(threadplay, fctl->plchan, 64 * 1024);
	threadcreate(threadfetch, fctl, 64 * 1024);
	return fctl;
}


void
usage(void)
{
	fprint(2, "usage: %s file.pcm\n", argv0);
	threadexitsall("usage");
}


void
clear(void)
{
	draw(screen, screen->r, Ibg, 0, ZP);
}


void
setdwidth(void)
{
	dwidth = Dx(screen->r) - 8;
}


void
drawpcm(Point p, ulong start)
{
	Rectangle r;
	r.min = p;
	r.max.x = r.min.x + fillrow(start, dwidth);
	r.max.y = r.min.y + DHEIGHT;
	draw(screen, r, Irow, 0, ZP);
	return;
}


Point
getcurxy()
{
	Point p;
	p = rbars.min;
	p.x += curpos%dwidth;
	p.y += (curpos/dwidth - scroll)*(DHEIGHT + 4);
	return p;
}


void
drawcur(Point p)
{
	draw(screen, Rpt(p, addpt(p, Pt(1, DHEIGHT))), Icur, 0, ZP);
}


void
drawcurabs(void)
{
	if (curpos < scroll *dwidth) return;
	drawcur(getcurxy());
}


void
clearcurabs(void)
{
	Point p;
	if (curpos < scroll *dwidth) return;
	p = getcurxy();
	fillrow(curpos, 2);
	draw(screen, Rpt(p, addpt(p, Pt(1, DHEIGHT))), Irow, 0, ZP);
}


void
redraw(ulong d)
{
	Point p;
	p = rbars.min;
	while (p.y < screen->r.max.y - (DHEIGHT)) {
		if (d > pcmlen/(4 * ZOOMOUT)) break;
		drawpcm(p, d);
		d += dwidth;
		p.y += DHEIGHT + 4;
	}
	drawcurabs();
}


void
loadpcm(char *path)
{
	long n;
	int fd;
	s8int buf[1024];
	fd = open(path, OREAD);
	pcmlen = 0;
	while((n = read(fd, buf, 1024)) > 0){
		pcm = realloc(pcm, pcmlen + n);
		memcpy(pcm + pcmlen, buf, n);
		pcmlen += n;
	}
	close(fd);
	pp = pcm;
}


int
fillrow(ulong start, ulong width)
{
	u8int min, max;
	uint dmin, dmax;
	long end, bsize;
	ulong n, i, j;
	u8int *buf, *bp;
	end = (start + width) * ZOOMOUT;
	if (end > pcmlen / 4) end = pcmlen / 4;
	bsize = width * Dy(Irow->r);
	buf = malloc(bsize);
	bp = buf;
	min = 0x7f;
	max = -0x7f;
	for (i=0, n=start*ZOOMOUT; (n<end)&&(bp<buf+width); n++, i++) {
		if (min > mono8[n]) min = mono8[n];
		if (max < mono8[n]) max = mono8[n];
		if (i >= ZOOMOUT) {
			i = 0;
			dmin = min * DHEIGHT / 256;
			dmax = max * DHEIGHT / 256;
			for (j = 0; j < dmin; j++) *(bp + j * width) = 0xff;
			for (j = dmin; j < dmax; j++) *(bp + j * width) = 0x00;
			for (j = dmax; j < Dy(Irow->r); j++) *(bp + j * width) = 0xff;
			min = 0x7f;
			max = -0x7f;
			bp++;
		}
	}
	Rectangle r;
	r = Irow->r;
	r.max.x = r.min.x + width;
	loadimage(Irow, r, buf, bsize);
	free(buf);
	return bp - buf;
}


u8int*
mkmono8(s8int *pcm, long pcmlen)
{
	long i, j;
	u8int *mono8;
	mono8 = malloc(sizeof(s8int) * pcmlen / 4);
	for (i = 0, j = 0; i < pcmlen; i+=4, j++) {
		mono8[j] = (pcm[i+1] + pcm[i+3] + 256) / 2;
	}
	return mono8;
}

void
drawscroll(int ds)
{
	Point p;
	long pos;
	scroll += ds;
	if (scroll < 0) {
		ds -= scroll;
		scroll = 0;
	}
	if (scroll > pcmlen / (4 * ZOOMOUT * dwidth)) {
		ds -=  scroll - pcmlen / (4 * ZOOMOUT * dwidth);
		scroll = pcmlen / (4 * ZOOMOUT * dwidth);
	}
	if (ds == 0) return;
	p = addpt(rbars.min, Pt(0, ds * (DHEIGHT+4)));
	draw(screen, rbars, screen, 0, p);
	if (ds < 0) {
		p = rbars.min;
		pos = dwidth * scroll;
		ds = - ds;
	} else {
		p = Pt(rbars.min.x, rbars.max.y - ds * (DHEIGHT + 4));
		pos = dwidth * (scroll - ds + Dy(rbars) / (DHEIGHT + 4));
		draw(screen, Rpt(p, rbars.max), Ibg, 0, ZP);
	}
	for (; ds > 0; ds--){
		drawpcm(p, pos);
		p.y += DHEIGHT + 4;
		pos += dwidth;
	}
	drawcurabs();
}