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
	PBufLen = 1024 * FrameSize,
	DHeight = 32,
	DCacheSize = 32,
	Margin = 4,

	MIdle = 0,
	MSelectStart = 1,
	MSelect = 2,

	PStop = 0,
	PPlay = 1,
	PPause = 2,
};

typedef struct DCache DCache;

struct DCache {
	int c;
	ulong start;
	Image *img;
};

int fillrow(ulong, ulong);
int row(int);
void drawpcm(Point, ulong);
void drawscroll(int);
void loadpcm(int);
void mexit(void);
void mplumb(void);
void mredraw(void);
void msnarf(void);
void mwrite(void);
void mzoom(void);
void redraw(ulong);
void resize(void);
void setselect(long, long);
void threadflush(void *);
void threadplay(void *);
void threadplumb(void *);
void threadselect(void *);
void usage(void);

DCache dcache[DCacheSize];
long pcmlen, monolen, scroll, smin, smax;
long Zoomout = 512;
Image *Ibg, *Itrbg, *Itrfg, *Irow;
Rectangle rbars;
int dwidth, needflush, maxlines, mmode;
char wpath[1024];
Mousectl *mctl;
Keyboardctl *kctl;
char *menustr[] = {"snarf", "plumb", "redraw", "write", "zoom", "exit", nil};
void (*menufunc[])(void) = {msnarf, mplumb, mredraw, mwrite, mzoom, mexit};
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
struct {
	char *path;
	s8int *p;
	int fid, state;
	vlong len, cur;
} play = {.path = "/dev/audio"};

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
	proccreate(threadplay, nil, 1024);

	lockdisplay(display);
	Ibg = allocimage(display, Rect(0,0,1,1), RGB24, 1, 0xBBBBBBFF);
	Itrbg = allocimage(display, Rect(0,0,1,1), RGB24, 1, DWhite);
	Itrfg = allocimage(display, Rect(0,0,1,1), RGB24, 1, DBlack);
	resize();
	Irow = allocimage(display, Rect(0, 0, dwidth, DHeight), CMAP8, 0, DBlue);
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
			if (kv == ' ') play.state = (play.state == PPlay) ? PPause : PPlay;
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
	if (plumb.recv < 0) return;
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
threadplay(void *)
{
	vlong n, pend;
	if ((play.fid = open(play.path, OWRITE)) < 0) {
		fprint(2, "%r\n");
		return;
	}
	play.p = malloc(PBufLen);
	if (play.p == nil) sysfatal("%r");
	for (;;) {
		switch (play.state) {
		case PStop:
			play.cur = smin * Zoomout * FrameSize;
		case PPause:
			sleep(1000/60);
			break;
		case PPlay:
			pend = (smin != smax) ? smax * Zoomout * FrameSize : pcm.len;
			n = (PBufLen < pend - play.cur) ? PBufLen : pend - play.cur;
			n = pread(pcm.fid, play.p, n, play.cur);
			n = write(play.fid, play.p, n);
			play.cur += n;
			if (play.cur >= pend) play.state = PStop;
		}
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
	lockdisplay(display);
	draw(screen, screen->r, Ibg, nil, ZP);
	unlockdisplay(display);
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
	int min, max, mono;
	uint dmin, dmax;
	long bsize;
	ulong n, i, j, rlen;
	u8int *buf, *bp;
	s8int *rbuf;
	bsize = width * Dy(Irow->r);
	rbuf = mallocz(width * FrameSize * Zoomout, 1);
	rlen = pread(pcm.fid, rbuf, width * FrameSize * Zoomout,
	  start * FrameSize * Zoomout);
	buf = malloc(bsize);
	bp = buf;
	min = 0xff;
	max = 0;
	for (i = 0, n = 0; (n < rlen) && (bp < buf + width); n += FrameSize, i++) {
		mono = (rbuf[n + 1] + rbuf[n + 3] + 0xff) / 2;
		if (min > mono) min = mono;
		if (max < mono) max = mono;
		if (i >= Zoomout) {
			i = 0;
			dmin = min * DHeight / 256;
			dmax = max * DHeight / 256;
			if (dmin == dmax) dmax ++;
			for (j = 0; j < dmin; j++) *(bp + j * width) = 0xff;
			for (j = dmin; j < dmax; j++) *(bp + j * width) = 0x00;
			for (j = dmax; j < Dy(Irow->r); j++) *(bp + j * width) = 0xff;
			min = 0xff;
			max = 0;
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
	return bp - buf;
}

int
row(int y)
{
	return (y - rbars.min.y) / (DHeight + Margin);
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
	n = enter("write to:", wpath, 1024, mctl, kctl, nil);
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
mzoom(void)
{
	int n;
	long nz;
	char *s = malloc(1024);
	snprint(s, 1024, "%ld", Zoomout);
	n = enter("zoom", s, 1024, mctl, kctl, nil);
	if (n <= 0) goto end;
	nz = strtol(s, nil, 10);
	if (nz > 0) {
		Zoomout = nz;
		redraw(scroll * dwidth);
	}
end:
	free(s);
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
	play.state = PStop;
	redraw(scroll * dwidth);
}
