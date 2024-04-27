#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <mouse.h>
#include <cursor.h>
#include <keyboard.h>

enum {
	DHeight = 32,
	DCacheSize = 32,
	Zoomout = 512,
	Margin = 4,
};

typedef struct DCache DCache;
struct DCache {
	int c;
	ulong start;
	Image *img;
};

DCache dcache[DCacheSize];

s8int *pcm;
u8int *mono8;
long pcmlen, scroll, smin, smax;
Image *Ibg, *Itrbg, *Itrfg, *Irow;
Rectangle rbars;
int fid, dwidth, needflush, maxlines;
char *path;

void usage(void);
void clear(void);
void drawpcm(Point, ulong);
void redraw(ulong);
void resize(void);
void loadpcm(int);
u8int* mkmono8(s8int*, long);
int fillrow(ulong, ulong);
int row(int);
void drawscroll(int);
void threadflush(void *);

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
	path = argv[0];
	fid = open(path, OREAD);
	loadpcm(fid);
	mono8 = mkmono8(pcm, pcmlen);

	if (argc <= 0) usage();
	if(initdraw(0, 0, "amp") < 0)
		sysfatal("inidraw failed: %r");
	if((mctl = initmouse(0, screen)) == nil)
		sysfatal("initmouse failed: %r");
	if((kctl = initkeyboard(0)) == nil)
		sysfatal("initkeyboard failed: %r");

	display->locking = 1;
	unlockdisplay(display);

	proccreate(threadflush, nil, 1024);

	lockdisplay(display);
	Ibg = allocimage(display, Rect(0,0,1,1), RGB24, 1, 0xBBBBBBFF);
	Itrbg = allocimage(display, Rect(0,0,1,1), RGB24, 1, DWhite);
	Itrfg = allocimage(display, Rect(0,0,1,1), RGB24, 1, DBlack);
	resize();
	Irow = allocimage(display, Rect(0, 0, dwidth, DHeight), CMAP8, 0, DBlue);
	clear();
	unlockdisplay(display);
	redraw(0);
	needflush = 1;
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
			if (kv == Kdown) drawscroll(maxlines/3);
			if (kv == Kup) drawscroll(-maxlines/3);
			if (kv == Kpgdown) drawscroll(maxlines);
			if (kv == Kpgup) drawscroll(-maxlines);
			break;
		case 1: /* mouse */
			//if (mv.buttons == 0);
			//if (mv.buttons == 1);
			//if (mv.buttons == 4);
			if (mv.buttons == 8) drawscroll(-row(mv.xy.y));
			if (mv.buttons == 16) drawscroll(row(mv.xy.y));
			break;
		case 2: /* resize */
			lockdisplay(display);
			if(getwindow(display, Refnone) < 0)
				sysfatal("resize failed: %r");
			resize();
			freeimage(Irow);
			Irow = allocimage(display, Rect(0, 0, dwidth, DHeight),
				CMAP8, 0, DBlue);
			clear();
			unlockdisplay(display);
			redraw(scroll * dwidth);
			needflush = 1;
			break;
		}
	}
}

void
threadflush(void *)
{
	threadsetname("flush");
	for (;;) {
		if (needflush != 0) {
			lockdisplay(display);
			flushimage(display, 1);
			unlockdisplay(display);
			needflush = 0;
		}
		sleep(1000/60);
	}
}

void
resize(void)
{
	dwidth = Dx(screen->r) - Margin * 2;
	rbars = screen->r;
	rbars.min.x += Margin;
	rbars.max.x -= Margin;
	rbars.min.y += Margin;
	rbars.max.y -= Margin;
	if (scroll > pcmlen/(4 * Zoomout*dwidth))
		scroll = pcmlen/(4 * Zoomout*dwidth);
	maxlines = Dy(rbars) / (DHeight + Margin);
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
	draw(screen, screen->r, Ibg, nil, ZP);
}

Image *
getmask(ulong start, ulong width)
{
	Rectangle r;
	r = Irow->r;
	r.max.x = r.min.x + fillrow(start, width);
	replclipr(Irow, 0, r);
	return Irow;
}

void
drawpcm(Point p, ulong start)
{
	Image *mask;
	Rectangle r;
	long w;
	mask = getmask(start, dwidth);
	r.min = p;
	r.max.x = p.x + Dx(mask->clipr);
	r.max.y = p.y + DHeight;

	lockdisplay(display);
	if (start < smin) {
		w = smin - start;
		if (w > Dx(r)) w = Dx(r);
		r.max.x = r.min.x + w;
		draw(screen, r, mask, 0, ZP);
		start += w;
		r.min.x += w;
		r.max.x = p.x + Dx(mask->clipr);
	}
	if (start < smax) {
		w = smax - start;
		if (w > Dx(r)) w = Dx(r);
		r.max.x = r.min.x + w;
		draw(screen, r, Itrbg, 0, ZP);
		draw(screen, r, Itrfg, mask, ZP);
		start += w;
		r.min.x += w;
		r.max.x = p.x + Dx(mask->clipr);
	}
	if ((start >= smax)) {
		draw(screen, r, mask, 0, ZP);
	}
	unlockdisplay(display);
	needflush = 1;

	return;
}

void
redraw(ulong d)
{
	Point p;
	p = rbars.min;
	while (p.y < screen->r.max.y) {
		if (d > pcmlen/(4 * Zoomout)) break;
		drawpcm(p, d);
		d += dwidth;
		p.y += DHeight + Margin;
	}
}

void
loadpcm(int fd)
{
	long n;
	s8int *buf;
	buf = malloc(32 * 1024);
	pcmlen = 0;
	while((n = read(fd, buf, 32 * 1024)) > 0){
		pcm = realloc(pcm, pcmlen + n);
		memcpy(pcm + pcmlen, buf, n);
		pcmlen += n;
	}
	free(buf);
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
	lockdisplay(display);
	loadimage(Irow, r, buf, bsize);
	unlockdisplay(display);
	free(buf);
	return bp - buf;
}

int
row(int y)
{
	return 1 + (y - rbars.min.y) / (DHeight + Margin);
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
	lockdisplay(display);
	clear();
	unlockdisplay(display);
	needflush = 1;
	redraw(scroll * dwidth);
}
