#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

#include "pages.h"

enum {
	FrameSize = sizeof(s16int) * 2,
};

PBuf pg;

struct {
	int min;
	int max;
	char str[12*3+1]; // see update_selstr
} sel;

void fs_open(Req *r);
void fs_read(Req *r);
void fs_write(Req *r);

void fcut_open(Req *r);
void fcut_write(Req *r);

void normalize_sel(void);
void update_selstr(void);

Srv ampsrv = {
	.read = fs_read,
	.write = fs_write,
	.open = fs_open,
};

File *fdata, *fcut, *fctl;
char *service, *mountpath = "/mnt/amp";

void
usage(void)
{
	fprint(2,"usage: %s\n", argv0);
	threadexitsall("usage");
}

void
threadmain(int argc, char **argv)
{
	ARGBEGIN {
	case 'D':
		chatty9p++;
		break;
	case 's':
		service = EARGF(usage());
		break;
	case 'm':
		mountpath = EARGF(usage());
		break;
	default: usage();
	} ARGEND
	if (argc != 0) usage();
	update_selstr();
	ampsrv.tree = alloctree("amp", "amp", DMDIR|0555, nil);
	fdata = createfile(ampsrv.tree->root, "data", "amp", 0666, nil);
	fcut = createfile(ampsrv.tree->root, "cut", "amp", 0666, nil);
	fctl = createfile(ampsrv.tree->root, "ctl", "amp", 0666, nil);
	threadpostmountsrv(&ampsrv, service, mountpath, MREPL);
}

void
fs_open(Req *r)
{
	char *rstr = nil;
	File *file = r->fid->file;
	uchar mode = r->ifcall.mode;
	if (file == fcut) {
		fcut_open(r);
		goto end;
	}
	if ((mode & OTRUNC) == 0) goto end;
	if (file == fctl) goto end;
	if (file == fdata) {
		pg.length = 0;
		fdata->length = 0;
		// TODO: free pages ???
		// probably better to do it on fs_close()
		goto end;
	}
	rstr = "what";
end:
	respond(r, rstr);
}

void
fs_read(Req *r)
{
	if (r->fid->file == fdata) {
		r->ofcall.count = pbread(&pg, r->ofcall.data,
		  r->ifcall.count, r->ifcall.offset);
		respond(r, nil);
	} else if (r->fid->file == fcut) {
		vlong min = sel.min * FrameSize;
		vlong max = sel.max * FrameSize;
		r->ifcall.offset += min;
		if (r->ifcall.offset + r->ifcall.count > max)
			r->ifcall.count = max - r->ifcall.offset;
		if (r->ifcall.offset > max) r->ifcall.count = 0;
		r->ofcall.count = pbread(&pg, r->ofcall.data,
		  r->ifcall.count, r->ifcall.offset);
		respond(r, nil);
	} else if (r->fid->file == fctl) {
		readstr(r, sel.str);
		respond(r, nil);
	} else {
		respond(r, "nope");
	}
}

void
fs_write(Req *r)
{
	if (r->fid->file == fdata) {
		r->ofcall.count = pbwrite(&pg, r->ifcall.data,
		  r->ifcall.count, r->ifcall.offset);
		if (pg.length > fdata->length) fdata->length = pg.length;
		update_selstr();
		respond(r, nil);
	} else if (r->fid->file == fcut) {
		fcut_write(r);
		update_selstr();
		respond(r, nil);
	} else if (r->fid->file == fctl) {
		int newmin, newmax;
		char *np, *rp;
		np = r->ifcall.data;
		newmin = strtol(np, &rp, 10);
		if (rp <= np) {
			respond(r, "bad first value");
			return;
		}
		newmax = strtol(rp, &rp, 10);
		if (rp <= np) {
			respond(r, "bad second value");
			return;
		}
		if (rp - np > r->ifcall.count) {
			respond(r, "somehow buffer overrun");
			return;
		}
		sel.min = newmin;
		sel.max = newmax;
		normalize_sel();
		update_selstr();
		fcut->length = (sel.max - sel.min) * FrameSize;
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
	} else {
		respond(r, "nope");
	}
}

void
fcut_open(Req *r)
{
	vlong min, max;
	min = sel.min * FrameSize;
	max = sel.max * FrameSize;

	if ((r->ifcall.mode & OTRUNC) != 0) {
		Page *maxpg = splitpage(&pg, max);
		Page *minpg = splitpage(&pg, min);
		assert(minpg != nil);
		assert(maxpg != nil);
		if (minpg->prev != nil) {
			minpg->prev->next = maxpg;
			maxpg->prev = minpg->prev;
		} else {
			pg.start = maxpg;
			maxpg->prev = nil;
		}
		while (minpg != maxpg) {
			Page *fp = minpg;
			fp->as->len = 0;
			minpg = fp->next;
			freepage(fp);
		}
		pg.length -= max - min;
		fdata->length = pg.length;
		fcut->length = 0;
		sel.max = sel.min;
		normalize_sel();
	}
}

void
fcut_write(Req *r)
{
	Page *maxpage;
	vlong min, max, offset;
	long count;
	min = sel.min * FrameSize;
	max = sel.max * FrameSize;

	maxpage = splitpage(&pg, max);
	if (maxpage == nil) {
		maxpage = addpage(&pg);
	}
	// insert more pages as needed
	offset = r->ifcall.offset + min;
	count = r->ifcall.count;
	while (offset + count > max) {
		long n = offset + count - max;
		if (n > PageSize) n = PageSize;
		Page *ins = allocpage();
		ins->as->len = n;
		ins->next = maxpage;
		ins->prev = maxpage->prev;
		if (maxpage->prev != nil) maxpage->prev->next = ins;
		else {
			pg.start = ins;
		}
		maxpage->prev = ins;
		max += n;
		pg.length  += n;
	}
	sel.max = max / FrameSize;
	normalize_sel();
	fcut->length = (sel.max - sel.min) * FrameSize;
	fdata->length = pg.length;
	// write to pagebuffer as usual
	r->ofcall.count = pbwrite(&pg, r->ifcall.data, count, offset);
}

void
normalize_sel(void)
{
	vlong bufmax = pg.length / FrameSize;
	if (sel.min > bufmax) sel.min = bufmax;
	if (sel.max > bufmax) sel.max = bufmax;
	if (sel.min > sel.max) {
		int x = sel.max;
		sel.max = sel.min;
		sel.min = x;
	}
}

void
update_selstr(void)
{
	snprint(sel.str, sizeof(sel.str), "%11d %11d %11d \n",
	  sel.min, sel.max, (int)(pg.length / FrameSize));
}
