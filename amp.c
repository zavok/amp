#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <mouse.h>
#include <cursor.h>
#include <keyboard.h>

#define DHeight 32
#define Zoomout 512

s8int *pcm, *pp;
u8int *mono8;
long pcmlen, scroll, curpos;
Image *Ibg, *Itrbg, *Itrfg, *Icur, *Irow;
Rectangle rbars;
int dwidth;

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
int fillrow(ulong, ulong);
void drawscroll(int);

void
threadmain(int argc, char **argv)
{
	Mousectl *mctl;
	Keyboardctl *kctl;
	Mouse mv;
	Rune  kv;
	int   rv[2];

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

	Ibg = allocimage(display, Rect(0,0,1,1), RGB24, 1, 0xBBBBBBFF);
	Itrbg = allocimage(display, Rect(0,0,1,1), RGB24, 1, DWhite);
	Itrfg = allocimage(display, Rect(0,0,1,1), RGB24, 1, DBlack);
	Icur = allocimage(display, Rect(0,0,1,1), RGB24, 1, DRed);

	curpos = 0;

	resize();
	Irow = allocimage(display, Rect(0, 0, dwidth, DHeight),
		CMAP8, 0, DBlue);
	clear();
	redraw(0);
	flushimage(display, 1);
	Alt alts[5] = {
		{kctl->c, &kv, CHANRCV},
		{mctl->c, &mv, CHANRCV},
		{mctl->resizec, rv, CHANRCV},
		{0, 0, CHANEND},
	};
	for (;;) {
		switch (alt(alts)) {
		case 0: /* keyboard */
			if (kv == 0x7f) threadexitsall(nil);
			break;
		case 1: /* mouse */
			if (mv.buttons == 0) {
			}
			if (mv.buttons == 1) {
				clearcurabs();
				long mrow = (mv.xy.y - screen->r.min.y) / (DHeight + 4);
				long mx = mv.xy.x - screen->r.min.x - 4;
				if (mx < 0) mx = 0;
				if (mx >= dwidth) mx = dwidth - 1;
				long newpos = (scroll + mrow) * dwidth + mx;
				curpos = newpos;
				if (curpos >= pcmlen / (Zoomout * 4))
					curpos = pcmlen / (Zoomout * 4) - 1;
				pp = pcm + curpos * (Zoomout * 4);
				drawcurabs();
				flushimage(display, 1);
			}
			if (mv.buttons == 4) {
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
			Irow = allocimage(display, Rect(0, 0, dwidth, DHeight),
				CMAP8, 0, DBlue);
			clear();
			redraw(scroll * dwidth);
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
	height = Dy(screen->r) / (DHeight + 4) * (DHeight + 4);
	rbars.min.y += 2 + (Dy(screen->r) - height) / 2;
	rbars.max.y = rbars.min.y + height;

	if (scroll > pcmlen/(4 * Zoomout*dwidth))
		scroll = pcmlen/(4 * Zoomout*dwidth);
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
	r.max.y = r.min.y + DHeight;
	draw(screen, r, Irow, 0, ZP);
	return;
}

Point
getcurxy()
{
	Point p;
	p = rbars.min;
	p.x += curpos%dwidth;
	p.y += (curpos/dwidth - scroll)*(DHeight + 4);
	return p;
}

void
drawcur(Point p)
{
	draw(screen, Rpt(p, addpt(p, Pt(1, DHeight))), Icur, 0, ZP);
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
	draw(screen, Rpt(p, addpt(p, Pt(1, DHeight))), Irow, 0, ZP);
}

void
redraw(ulong d)
{
	Point p;
	p = rbars.min;
	while (p.y < screen->r.max.y - (DHeight)) {
		if (d > pcmlen/(4 * Zoomout)) break;
		drawpcm(p, d);
		d += dwidth;
		p.y += DHeight + 4;
	}
	drawcurabs();
}

void
loadpcm(char *path)
{
	long n;
	int fd;
	s8int *buf;
	buf = malloc(32 * 1024);
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
	end = (start + width) * Zoomout;
	if (end > pcmlen / 4) end = pcmlen / 4;
	bsize = width * Dy(Irow->r);
	buf = malloc(bsize);
	bp = buf;
	min = 0x7f;
	max = -0x7f;
	for (i=0, n=start*Zoomout; (n<end)&&(bp<buf+width); n++, i++) {
		if (min > mono8[n]) min = mono8[n];
		if (max < mono8[n]) max = mono8[n];
		if (i >= Zoomout) {
			i = 0;
			dmin = min * DHeight / 256;
			dmax = max * DHeight / 256;
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
	if (scroll > pcmlen / (4 * Zoomout * dwidth)) {
		ds -=  scroll - pcmlen / (4 * Zoomout * dwidth);
		scroll = pcmlen / (4 * Zoomout * dwidth);
	}
	if (ds == 0) return;
	p = addpt(rbars.min, Pt(0, ds * (DHeight+4)));
	draw(screen, rbars, screen, 0, p);
	if (ds < 0) {
		p = rbars.min;
		pos = dwidth * scroll;
		ds = - ds;
	} else {
		p = Pt(rbars.min.x, rbars.max.y - ds * (DHeight + 4));
		pos = dwidth * (scroll - ds + Dy(rbars) / (DHeight + 4));
		draw(screen, Rpt(p, rbars.max), Ibg, 0, ZP);
	}
	for (; ds > 0; ds--){
		drawpcm(p, pos);
		p.y += DHeight + 4;
		pos += dwidth;
	}
	drawcurabs();
}
