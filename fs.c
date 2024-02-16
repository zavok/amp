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

void normalize_sel(void);
void update_selstr(void);

Srv ampsrv = {
	.read = fs_read,
	.write = fs_write,
	//.create = fs_create,
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
	if ((mode & OTRUNC) == 0) goto end;
	if (file == fctl) goto end;
	if (file == fcut) {
		rstr = "trunc not implemented yet";
		goto end;
	}
	if (file == fdata) {
		pg.length = 0;
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
		respond(r, nil);
	} else if (r->fid->file == fcut) {
		respond(r, "fcut nope");
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
