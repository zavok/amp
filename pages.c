#include <u.h>
#include <libc.h>

#include "pages.h"

Array *
allocarray(long len)
{
	Array *new = malloc(sizeof(ArHeader) + len);
	assert(new != nil);
	new->len = len;
	new->ref = 0;
	return new;
}

ArSlice *
allocarslice(Array *ar, long start, long len)
{
	ArSlice *new = malloc(sizeof(ArSlice));
	new->ar = ar;
	new->p = ar->p + start;
	new->len = len;
	new->cap = ar->len - start;
	assert(start + len <= ar->len);
	return new;
}

Page *
allocpage(void)
{
	Array *ar = allocarray(PageSize);
	memset(ar->p, '?', PageSize);
	ArSlice *as = allocarslice(ar, 0, PageSize);
	ar->ref++;
	Page *new = malloc(sizeof(Page));
	new->as = as;
	new->prev = nil;
	new->next = nil;
	return new;
}

Page *
duppage(Page *old)
{
	Page *new = malloc(sizeof(Page));
	ArSlice *as = malloc(sizeof(ArSlice));
	new->as = as;
	new->prev = old->prev;
	new->next = old->next;
	as->ar = old->as->ar;
	as->p = old->as->p;
	as->len = old->as->len;
	as->cap = old->as->cap;
	as->ar->ref++;
	return new;
}

void
freepage(Page *pg)
{
	pg->as->ar->ref--;
	if (pg->as->ar->ref <= 0) {
		free(pg->as->ar);
	}
	free(pg->as);
	free(pg);
}

long
pbwrite(PBuf *pb, void *buf, long nbytes, vlong offset)
{
	Page *pgpt;
	long n, nwritten = 0;

	// TODO: move this section to fs.c/fwrite/fdata
	// or create separate pbgrow() function
	// also, last page's len should be shortened accordingly
	// [[[
	while (offset + nbytes > pb->size) {
		addpage(pb);
	}
	if (pb->length < offset + nbytes) pb->length = offset + nbytes;
	// ]]]

	pgpt = pb->start;
	if (offset >= pb->length) nbytes = 0;
	if (offset + nbytes >= pb->length) {
		nbytes = (pb->length - offset);
	}
	while (nbytes > 0) {
		if (pgpt == nil) {
			break;
		}
		if (offset >= pgpt->as->len) {
			offset -= pgpt->as->len;
			pgpt = pgpt->next;
			continue;
		}
		n = nbytes;
		if (pgpt->as->len - offset < n) n = pgpt->as->len - offset;
		memcpy(pgpt->as->p + offset, buf, n);
		nwritten += n;
		buf = (char *)buf + n;
		offset += n;
		nbytes -= n;
	}
	return nwritten;
}

long
pbread(PBuf *pb, void *buf, long nbytes, vlong offset)
{
	Page *pgpt = pb->start;
	long n, nread = 0;
	if (offset >= pb->length) nbytes = 0;
	if (offset + nbytes >= pb->length) {
		nbytes = (pb->length - offset);
	}
	while (nbytes > 0) {
		if (pgpt == nil) {
			break;
		}
		if (offset >= pgpt->as->len) {
			offset -= pgpt->as->len;
			pgpt = pgpt->next;
			continue;
		}
		n = nbytes;
		if (pgpt->as->len - offset < n) n = pgpt->as->len - offset;
		memcpy(buf, pgpt->as->p + offset, n);
		nread += n;
		buf = (char *)buf + n;
		offset += n;
		nbytes -= n;
	}
	return nread;
}

Page *
addpage(PBuf *pb)
{
	Page *new = allocpage();
	if ((pb->start == nil) || (pb->end == nil)) {
		pb->start = new;
		pb->end = new;
	} else {
		new->prev = pb->end;
		pb->end->next = new;
		pb->end = new;
	}
	pb->size += new->as->len;
	return new;
}

Page *
splitpage(PBuf *pb, vlong offset)
{
	vlong d, count = 0;
	Page *sp = pb->start;
	// find page at offset
	while ((sp != nil) && (count + sp->as->len < offset)) {
		count += sp->as->len;
		sp = sp->next;
	}
	if (sp == nil) return nil;
	d = offset - count;
	if (d > 0) {
		Page *np = duppage(sp);
		np->as->len = d;
		np->next = sp;
		if (np->prev != nil) np->prev->next = np;
		else pb->start = np;
		sp->as->len -= d;
		sp->as->cap -= d;
		sp->as->p += d;
		sp->prev = np;
	}
	return sp;
}
