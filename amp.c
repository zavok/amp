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
	MaskCacheSize = 256,
	ScrollBarWidth = 12,
	Margin = 4,

	PStop = 0,
	PPlay = 1,
	PPause = 2,
};

Image * getmask(int);
long decoord(Point);
int row(int);
void clearcursor(void);
void clearmask(void);
void drawcursor(void);
void drawmask(Image *, int);
void drawpcm(Point, int);
void drawscroll(int);
void drawscrollbar(void);
void loadpcm(int);
void mouseidle(Mouse);
void mousescroll(Mouse);
void mouseselect(Mouse);
void mexit(void);
void mplumb(void);
void mredraw(void);
void msnarf(void);
void mwrite(void);
void mzoom(void);
void redraw(void);
void resize(void);
void setselect(long, long);
void threadflush(void *);
void threadplay(void *);
void threadplumb(void *);
void usage(void);

long monolen, scroll, smin, smax, ss, se, Zoomout = 512;
Image *Ibg, *Itrbg, *Itrfg, *Icur;
Rectangle rscroll, rbars;
int dwidth, needflush, maxbars;
char wpath[1024];
Mousectl *mctl;
Keyboardctl *kctl;
char *menustr[] = {"snarf", "plumb", "redraw", "write", "zoom", "exit", nil};
void (*menufunc[])(void) = {msnarf, mplumb, mredraw, mwrite, mzoom, mexit};
void (*mousefp)(Mouse) = mouseidle;
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
struct {
	Image *img[MaskCacheSize];
	int id[MaskCacheSize];
} mask;

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
	proccreate(threadplumb, nil, 1024);
	proccreate(threadplay, nil, 1024);

	lockdisplay(display);
	Ibg = allocimage(display, Rect(0,0,1,1), RGB24, 1, 0xBBBBBBFF);
	Itrbg = allocimage(display, Rect(0,0,1,1), RGB24, 1, DWhite);
	Itrfg = allocimage(display, Rect(0,0,1,1), RGB24, 1, DBlack);
	Icur = allocimage(display, Rect(0,0,1,1), RGB24, 1, DRed);
	resize();
	clearmask();
	unlockdisplay(display);
	redraw();
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
			if (kv == Kdown) drawscroll(maxbars/3);
			if (kv == Kup) drawscroll(-maxbars/3);
			if (kv == Kpgdown) drawscroll(maxbars);
			if (kv == Kpgup) drawscroll(-maxbars);
			if (kv == ' ') play.state = (play.state == PPlay) ? PPause : PPlay;
			break;
		case 1: /* mouse */
			mousefp(mv);
			break;
		case 2: /* resize */
			lockdisplay(display);
			if(getwindow(display, Refnone) < 0)
				sysfatal("resize failed: %r");
			resize();
			clearmask();
			unlockdisplay(display);
			redraw();
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
			clearcursor();
			play.cur += n;
			drawcursor();
			if (play.cur >= pend) play.state = PStop;
			needflush = 1;
		}
	}
}

void
mouseidle(Mouse mv)
{
	int n;
	if ((mv.buttons != 0) && (ptinrect(mv.xy, rscroll) != 0)) {
		mousefp = mousescroll;
		mousescroll(mv);
	} else switch(mv.buttons) {
	case 1:
		mousefp = mouseselect;
		ss = decoord(mv.xy) / (Zoomout * FrameSize);
		mouseselect(mv);
		break;
	case 4:
		n = menuhit(3, mctl, &menu, nil);
		if (n >= 0) menufunc[n]();
		break;
	case 8:
		drawscroll(-1-row(mv.xy.y));
		break;
	case 16:
		drawscroll(1+row(mv.xy.y));
		break;
	}
}

void
mousescroll(Mouse mv)
{
	int tl;
	switch(mv.buttons) {
	case 0:
		mousefp = mouseidle;
		break;
	case 1:
		drawscroll(-1-row(mv.xy.y));
		break;
	case 2:
		tl = pcm.len / (FrameSize * Zoomout * dwidth);
		scroll = tl * (mv.xy.y - rscroll.min.y) / Dy(rscroll);
		if (scroll > tl) scroll = tl;
		if (scroll < 0) scroll = 0;
		redraw();
		needflush = 1;
		break;
	case 4:
		drawscroll(1+row(mv.xy.y));
		break;
	case 8:
		drawscroll(-1-row(mv.xy.y));
		break;
	case 16:
		drawscroll(1+row(mv.xy.y));
		break;
	}
}

void
mouseselect(Mouse mv)
{
	if (mv.buttons == 0) {
		mousefp = mouseidle;
		return;
	}
	se = decoord(mv.xy) / (Zoomout * FrameSize);
	setselect(ss, se);
}

void
resize(void)
{
	rscroll = screen->r;
	rscroll.min.x += Margin;
	rscroll.min.y += Margin;
	rscroll.max.x = rscroll.min.x + ScrollBarWidth;
	rscroll.max.y -= Margin;

	rbars = screen->r;
	rbars.min.x += 2 * Margin + ScrollBarWidth;
	rbars.max.x -= Margin;
	rbars.min.y += Margin;
	rbars.max.y -= Margin;
	dwidth = Dx(rbars);
	if (scroll > pcm.len/(FrameSize * Zoomout * dwidth))
		scroll = pcm.len/(FrameSize * Zoomout * dwidth);
	maxbars = Dy(rbars) / (DHeight + Margin);
}

void
usage(void)
{
	fprint(2, "usage: %s file.pcm\n", argv0);
	threadexitsall("usage");
}

Image *
getmask(int n)
{
	int m;
	m = n % MaskCacheSize;
	if (mask.id[m] != n) {
		mask.id[m] = n;
		if (mask.img[m] == nil) {
			mask.img[m] = allocimage(display, Rect(0, 0, dwidth, DHeight),
			  CMAP8, 0, DBlue);
		}
		drawmask(mask.img[m], n);
	}
	return mask.img[m];
}

void
drawpcm(Point p, int n)
{
	Image *mask;
	Rectangle r;
	long w;
	ulong start;
	mask = getmask(n);
	r.min = p;
	r.max.x = p.x + Dx(mask->clipr);
	r.max.y = p.y + DHeight;
	if (r.max.y > rbars.max.y) r.max.y = rbars.max.y;
	start = n * dwidth;
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
redraw(void)
{
	int d;
	Point p;
	p = rbars.min;
	lockdisplay(display);
	draw(screen, screen->r, Ibg, nil, ZP);
	unlockdisplay(display);
	drawscrollbar();
	d = scroll;
	while (p.y < screen->r.max.y) {
		if (d * dwidth > pcm.len / (FrameSize * Zoomout)) break;
		drawpcm(p, d);
		d++;
		p.y += DHeight + Margin;
	}
	drawcursor();
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

void
drawmask(Image *mask, int bn)
{
	Rectangle r;
	int min, max, mono;
	long bsize;
	s8int *rbuf;
	u8int *buf, *bp;
	uint dmin, dmax;
	ulong n, i, j, rlen, FZ, start;
	start = bn * dwidth;
	FZ = FrameSize * Zoomout;
	rbuf = malloc(dwidth * FZ);
	rlen = pread(pcm.fid, rbuf, dwidth * FZ, start * FZ);
	bsize = dwidth * DHeight;
	buf = mallocz(bsize, 1);
	bp = buf;
	min = 0xff;
	max = 0;
	i = 0;
	for (n = 0; (n < rlen) && (bp < buf + dwidth); n += FrameSize) {
		mono = (rbuf[n + 1] + rbuf[n + 3] + 0xff) / 2;
		if (min > mono) min = mono;
		if (max < mono) max = mono;
		if (i >= Zoomout - 1) {
			i = 0;
			dmin = min * DHeight / 256;
			dmax = max * DHeight / 256;
			if (dmin == dmax) dmax++;
			for (j = 0; j < dmin; j++) *(bp + j * dwidth) = 0xff;
			for (j = dmin; j < dmax; j++) *(bp + j * dwidth) = 0x00;
			for (j = dmax; j < Dy(mask->r); j++) *(bp + j * dwidth) = 0xff;
			min = 0xff;
			max = 0;
			bp++;
		} else i++;
	}
	r = mask->r;
	r.max.x = r.min.x + rlen / FZ;
	lockdisplay(display);
	loadimage(mask, mask->r, buf, bsize);
	replclipr(mask, 0, r);
	unlockdisplay(display);
	free(buf);
	free(rbuf);
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
	redraw();
	needflush = 1;
}

void
drawscrollbar(void)
{
	Rectangle r, br;
	int tl, offset, width;
	r = rscroll;

	tl = pcm.len / (FrameSize * Zoomout * dwidth);

	offset = scroll * Dy(rbars) / tl;
	width = maxbars * Dy(rbars) / tl;
	
	br = Rect(
	  r.min.x,
	  r.min.y + offset,
	  r.max.x,
	  r.min.y + offset + width);
	if (br.max.y > r.max.y) br.max.y = r.max.y;
	lockdisplay(display);
	draw(screen, r, Itrbg, nil, ZP);
	draw(screen, br, Itrfg, nil, ZP);
	unlockdisplay(display);
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
	clearmask();
	redraw();
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
		clearmask();
		redraw();
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
	play.cur = smin * FrameSize * Zoomout;
	redraw();
}

void
clearcursor(void)
{
	int b, n, m;
	Rectangle r;
	Image *mask, *bg, *fg;
	b = play.cur / (FrameSize * Zoomout);
	n = b / dwidth;
	m = b % dwidth;
	if ((n < scroll) || (n >= scroll + maxbars)) return;
	mask = getmask(n);

	n -= scroll;
	r.min.y = rbars.min.y + n * (DHeight + Margin);
	r.max.y = r.min.y + DHeight;
	r.min.x = rbars.min.x + m;
	r.max.x = r.min.x + 1;

	if ((b < smin) || (b >= smax)) {
		bg = Itrfg;
		fg = Itrbg;
	} else {
		bg = Itrbg;
		fg = Itrfg;
	}
	lockdisplay(display);
	draw(screen, r, bg, 0, ZP);
	draw(screen, r, fg, mask, Pt(m, 0));
	unlockdisplay(display);
	needflush = 1;
}

void
drawcursor(void)
{
	int b, n, m;
	Rectangle r;
	b = play.cur / (FrameSize * Zoomout);
	n = b / dwidth;
	m = b % dwidth;
	if ((n < scroll) || (n >= scroll + maxbars)) return;
	n -= scroll;
	r.min.y = rbars.min.y + n * (DHeight + Margin);
	r.max.y = r.min.y + DHeight;
	r.min.x = rbars.min.x + m;
	r.max.x = r.min.x + 1;


	lockdisplay(display);
	draw(screen, r, Icur, 0, ZP);
	unlockdisplay(display);
	needflush = 1;
}

void
clearmask(void)
{
	int i;
	for (i = 0; i < MaskCacheSize; i++) {
		mask.id[i] = -1;
		if (mask.img[i] != nil) {
			freeimage(mask.img[i]);
			mask.img[i] = nil;
		}
	}
}

long
decoord(Point xy)
{
	long p;
	p = ((scroll + row(xy.y)) * dwidth + xy.x - rbars.min.x) * Zoomout * FrameSize;
	if (p < 0) p = 0;
	if (p > pcm.len) {
		p = pcm.len;
	}
	return p;
}
