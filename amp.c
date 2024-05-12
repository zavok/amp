#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <mouse.h>
#include <cursor.h>
#include <keyboard.h>
#include <plumb.h>

enum {
	FrameSize = sizeof(s16int) * 2,
	DHeight = 32,
	DCacheSize = 32,
	Zoomout = 512,
	Margin = 4,

	MIdle = 0,
	MSelectStart = 1,
	MSelect = 2,
};

typedef struct DCache DCache;

struct DCache {
	int c;
	ulong start;
	Image *img;
};

void usage(void);
void clear(void);
void drawpcm(Point, ulong);
void redraw(ulong);
void resize(void);
void loadpcm(int);
long mkmono8(u8int **, s8int*, long);
int fillrow(ulong, ulong);
int row(int);
void drawscroll(int);
void threadflush(void *);
void threadselect(void *);
void threadplumb(void *);
void msnarf(void);
void mplumb(void);
void mredraw(void);
void mwrite(void);
void mexit(void);
void setselect(long, long);

DCache dcache[DCacheSize];
long pcmlen, monolen, scroll, smin, smax;
Image *Ibg, *Itrbg, *Itrfg, *Irow;
Rectangle rbars;
int dwidth, needflush, maxlines, mmode;
char wpath[256];
Mousectl *mctl;
Keyboardctl *kctl;
char *menustr[] = {"snarf", "plumb", "redraw", "write", "exit", nil};
void (*menufunc[])(void) = {msnarf, mplumb, mredraw, mwrite, mexit};
Menu menu = {menustr, nil, 0};
struct {
	int send, recv;
	char *port;
} plumb = {.port = "amp"};
struct {
	s8int *p;
	int fid, len, mtime;
	char *path;
} pcm;

void
threadmain(int argc, char **argv)
{
	Mouse mv;
	Rune  kv;
	int   rv[2];

	ARGBEGIN{
	default:
		usage();
	}ARGEND;

	if (argc == 0) usage();
	pcm.path = argv[0];
	pcm.fid = open(pcm.path, OREAD);
	loadpcm(pcm.fid);
	monolen = pcm.len / FrameSize;

	plumb.send = plumbopen("send", OWRITE);
	if (plumb.send < 0) fprint(2, "%r\n");
	plumb.recv = plumbopen(plumb.port, OREAD);
	if (plumb.recv <0) fprint(2, "%r\n");

	if(initdraw(0, 0, "amp") < 0)
		sysfatal("inidraw failed: %r");
	if((mctl = initmouse(0, screen)) == nil)
		sysfatal("initmouse failed: %r");
	if((kctl = initkeyboard(0)) == nil)
		sysfatal("initkeyboard failed: %r");

	display->locking = 1;
	unlockdisplay(display);

	proccreate(threadflush, nil, 1024);
	threadcreate(threadselect, &mv, 1024);
	proccreate(threadplumb, nil, 1024);

	lockdisplay(display);
	Ibg = allocimage(display, Rect(0,0,1,1), RGB24, 1, 0xBBBBBBFF);
	Itrbg = allocimage(display, Rect(0,0,1,1), RGB24, 1, DWhite);
	Itrfg = allocimage(display, Rect(0,0,1,1), RGB24, 1, DBlack);
	resize();
	Irow = allocimage(display, Rect(0, 0, dwidth, DHeight), CMAP8, 0, DBlue);
	clear();
	unlockdisplay(display);
	redraw(0);
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
			if (mv.buttons == 0) mmode = MIdle;
			if ((mv.buttons == 1) && (mmode == MIdle)) mmode = MSelectStart;
			if (mv.buttons == 4) {
				int n = menuhit(3, mctl, &menu, nil);
				if (n >= 0) menufunc[n]();
			}
			if (mv.buttons == 8) drawscroll(-1-row(mv.xy.y));
			if (mv.buttons == 16) drawscroll(1+row(mv.xy.y));
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
threadselect(void *v)
{
	int p;
	static ulong ss, se;
	Mouse *mv;
	threadsetname("select");
	mv = v;
	for (;;) {
		p = (scroll + row(mv->xy.y)) * dwidth + mv->xy.x - rbars.min.x;
		if (p < 0) p = 0;
		if (p > monolen / Zoomout) {
			p = monolen / Zoomout;
		}
		switch (mmode) {
		case MSelectStart:
			mmode = MSelect;
			ss = p;
		case MSelect:
			se = p;
			setselect(ss, se);
		}
		yield();
	}
}

void
threadplumb(void *)
{
	Plumbmsg *m;
	long s, e;
	threadsetname("plumb");
	for (;;) {
		m = plumbrecv(plumb.recv);
		if (m->ndata < 24) fprint(2, "plumb message too short\n");
		else if (strcmp(m->src, "amp") != 0){
			s = strtol(m->data, nil, 10);
			e = strtol(m->data + 12, nil, 10);
			setselect(s / (Zoomout * FrameSize), e / (Zoomout * FrameSize));
		}
		plumbfree(m);
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
	if (scroll > pcm.len/(FrameSize * Zoomout * dwidth))
		scroll = pcm.len/(FrameSize * Zoomout * dwidth);
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
		draw(screen, r, Itrfg, mask, Pt(r.min.x - p.x, 0));
		start += w;
		r.min.x += w;
		r.max.x = p.x + Dx(mask->clipr);
	}
	if ((start >= smax)) {
		draw(screen, r, mask, 0, Pt(r.min.x - p.x, 0));
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
		if (d > pcm.len/(FrameSize * Zoomout)) break;
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
	pcm.len = 0;
	while((n = read(fd, buf, 32 * 1024)) > 0){
		pcm.p = realloc(pcm.p, pcm.len + n);
		memcpy(pcm.p + pcm.len, buf, n);
		pcm.len += n;
	}
	free(buf);
}

int
fillrow(ulong start, ulong width)
{
	u8int min, max;
	uint dmin, dmax;
	long bsize;
	ulong n, i, j, rlen, ulen;
	u8int *buf, *bp;
	s8int *rbuf;
	u8int *ubuf;
	bsize = width * Dy(Irow->r);
	rbuf = malloc(width * FrameSize * Zoomout);
	rlen = pread(pcm.fid, rbuf, width * FrameSize * Zoomout,
	  start * FrameSize * Zoomout);
	ulen = mkmono8(&ubuf, rbuf, rlen);
	buf = malloc(bsize);
	bp = buf;
	min = 0x7f;
	max = -0x7f;
	for (i = 0, n = 0; (n < ulen) && (bp < buf + width); n++, i++) {
		int mono;
		mono = ubuf[n];
		if (min > mono) min = mono;
		if (max < mono) max = mono;
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
	free(rbuf);
	free(ubuf);
	return bp - buf;
}

int
row(int y)
{
	return (y - rbars.min.y) / (DHeight + Margin);
}

long
mkmono8(u8int **mono8, s8int *pcm, long pcmlen)
{
	long i, j;
	long monolen;
	monolen = pcmlen / FrameSize;
	*mono8 = malloc(monolen);
	for (i = 0, j = 0; i < pcmlen; i += FrameSize, j++) {
		(*mono8)[j] = (pcm[i+1] + pcm[i+3] + 256) / 2;
	}
	return monolen;
}

void
drawscroll(int ds)
{
	scroll += ds;
	if (scroll < 0) {
		ds -= scroll;
		scroll = 0;
	}
	if (scroll > monolen / (Zoomout * dwidth)) {
		ds -=  scroll - monolen / (Zoomout * dwidth);
		scroll = monolen / (Zoomout * dwidth);
	}
	if (ds == 0) return;
	lockdisplay(display);
	clear();
	unlockdisplay(display);
	needflush = 1;
	yield();
	redraw(scroll * dwidth);
	needflush = 1;
}

void
msnarf(void)
{
	int fd, n, min, max;
	char buf[25];
	min = smin * Zoomout * FrameSize;
	max = smax * Zoomout * FrameSize;
	n = snprint(buf, 25, "%11d %11d ", min, max);
	if ((fd = open("/dev/snarf", OWRITE)) < 0) {
		fprint(2, "%r\n");
		return;
	}
	write(fd, buf, n);
	close(fd);
}

void
mplumb(void)
{
	int min, max;
	min = smin * Zoomout * FrameSize;
	max = smax * Zoomout * FrameSize;
	Plumbmsg *m;
	m = mallocz(sizeof(Plumbmsg), 1);
	m->src = smprint("amp");
	m->dst = strdup(plumb.port);
	m->type = strdup("text");
	m->data = smprint("%11d %11d ", min, max);
	m->ndata = strlen(m->data);
	plumbsend(plumb.send, m);
	plumbfree(m);
}

void
mredraw(void)
{
	redraw(scroll * dwidth);
}

void
mwrite(void)
{
	int n, fd;
	long min, max;
	n = enter("write to:", wpath, 256, mctl, kctl, nil);
	if (n <= 0) return;
	if ((fd = create(wpath, OWRITE, 0666)) < 0) {
		fprint(2, "%r\n");
		return;
	}
	min = smin * Zoomout * FrameSize;
	max = smax * Zoomout * FrameSize;
	write(fd, pcm.p + min, max - min);
	close(fd);
}

void
mexit(void)
{
	threadexitsall(nil);
}

void
setselect(long s, long e)
{
	if (s < e) smin = s, smax = e;
	else smax = s, smin = e;
	redraw(scroll * dwidth);
}

