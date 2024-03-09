#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <mouse.h>
#include <cursor.h>
#include <keyboard.h>

enum {
	DHeight = 32,
	Zoomout = 512,
	Margin = 4,
};

s8int *pcm, *pp;
u8int *mono8;
long pcmlen, scroll;
Image *Ibg, *Itrbg, *Itrfg, *Irow;
Rectangle rbars;
int fid, dwidth, needflush;
char *path;

void usage(void);
void clear(void);
void setdwidth(void);
void drawpcm(Point, ulong);
void drawcur(Point);
void redraw(ulong);
void resize(void);
void loadpcm(int);
u8int* mkmono8(s8int*, long);
int fillrow(ulong, ulong);
void drawscroll(int);
void scrollup(void);
void scrolldown(void);
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
	redraw(0);
	flushimage(display, 1);
	unlockdisplay(display);

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
			//if (mv.buttons == 0);
			//if (mv.buttons == 1);
			//if (mv.buttons == 4);
			if (mv.buttons == 8) scrollup();
			if (mv.buttons == 16) scrolldown();
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
			redraw(scroll * dwidth);
			flushimage(display, 1);
			unlockdisplay(display);
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
	setdwidth();
	rbars = screen->r;
	rbars.min.x += Margin;
	rbars.max.x -= Margin;
	rbars.min.y += Margin;
	rbars.max.y -= Margin;
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
	dwidth = Dx(screen->r) - 2 * Margin;
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
	fprint(2, "loading pcm...");
	buf = malloc(32 * 1024);
	pcmlen = 0;
	while((n = read(fd, buf, 32 * 1024)) > 0){
		pcm = realloc(pcm, pcmlen + n);
		memcpy(pcm + pcmlen, buf, n);
		pcmlen += n;
	}
	pp = pcm;
	fprint(2, "done\n");
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
	lockdisplay(display);
	draw(screen, rbars, screen, 0, p);
	unlockdisplay(display);
	needflush = 1;
	if (ds < 0) {
		p = rbars.min;
		pos = dwidth * scroll;
		ds = - ds;
	} else {
		p = Pt(rbars.min.x, rbars.max.y - ds * (DHeight + Margin));
		pos = dwidth * (scroll - ds + Dy(rbars) / (DHeight + Margin));
		lockdisplay(display);
		draw(screen, Rpt(p, rbars.max), Ibg, 0, ZP);
		unlockdisplay(display);
		needflush = 1;
	}
	for (; ds > 0; ds--){
		lockdisplay(display);
		drawpcm(p, pos);
		unlockdisplay(display);
		needflush = 1;
		p.y += DHeight + Margin;
		pos += dwidth;
	}
}

void
scrollup(void)
{
	drawscroll(-1);
}

void
scrolldown(void)
{
	drawscroll(1);
}
