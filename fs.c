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
	char str[32];
} sel;

void fs_read(Req *r);
void fs_write(Req *r);

void normalize_sel(void);
void update_selstr(void);

Srv ampsrv = {
	.read = fs_read,
	.write = fs_write,
	// .open/.create (???) for handling truncating
};

File *fdata, *fcut, *fctl; 

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
	default: usage();
	} ARGEND
	if (argc != 0) usage();

	update_selstr();

	ampsrv.tree = alloctree("amp", "amp", DMDIR|0555, nil);
	fdata = createfile(ampsrv.tree->root, "data", "amp", 0666, nil);
	fcut = createfile(ampsrv.tree->root, "cut", "amp", 0666, nil);
	fctl = createfile(ampsrv.tree->root, "ctl", "amp", 0666, nil);
	threadpostmountsrv(&ampsrv, "amp", "/mnt/amp", MREPL);
}

void
fs_read(Req *r)
{
	if (r->fid->file == fdata) {
		r->ofcall.count = pbread(&pg, r->ofcall.data,
		  r->ifcall.count, r->ifcall.offset);
		respond(r, nil);
	} else if (r->fid->file == fcut) {
		respond(r, "fcut nope");
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
		if (pg.count > fdata->length) fdata->length = pg.count;
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
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
	} else {
		respond(r, "nope");
	}
}

void
normalize_sel(void)
{
	if (sel.min > sel.max) {
		int x = sel.max;
		sel.max = sel.min;
		sel.min = x;
	};
	// TODO: should check max is not bigger than total buffer size
}

void
update_selstr(void)
{
	snprint(sel.str, sizeof(sel.str), "%11d %11d ", sel.min, sel.max);
}
