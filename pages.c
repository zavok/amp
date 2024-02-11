#include <u.h>
#include <libc.h>

#include "pages.h"

Page *
allocpage(void)
{
	Page *new = mallocz(sizeof(Page), 1);
	new->count = PageSize;
	new->buf = mallocz(PageSize, 1);
	return new;
}

long
pbwrite(PBuf *pb, void *buf, long nbytes, vlong offset)
{
	Page *pgpt;
	vlong page_offset = 0;
	long n, nwritten = 0;
	while (offset + nbytes > pb->size) {
		addpage(pb);
	}
	pgpt = pb->start;
	if (pb->count < offset + nbytes) pb->count = offset + nbytes;
	while (nbytes > 0) {
		assert(page_offset <= offset);
		if (page_offset + pgpt->count > offset) {
			n = page_offset + pgpt->count - offset;
			if (n > nbytes) n = nbytes;
			memcpy(pgpt->buf, buf, n);
			nwritten += n;
			buf = (char *)buf + n;
			offset +=n;
			nbytes -= n;
		}
		page_offset += pgpt->count;
		pgpt = pgpt->next;
	}
	if (offset > pb->size) {
		pb->size = offset;
	}
	return nwritten;
}

long
pbread(PBuf *pb, void *buf, long nbytes, vlong offset)
{
	Page *pgpt = pb->start;
	long n, nread = 0;
	if (offset >= pb->count) nbytes = 0;
	if (offset + nbytes >= pb->count) {
		nbytes = (pb->count - offset);
	}
	while (nbytes > 0) {
		if (pgpt == nil) {
			break;
		}
		if (offset >= pgpt->count) {
			offset -= pgpt->count;
			pgpt = pgpt->next;
			continue;
		}
		n = nbytes;
		if (pgpt->count - offset < n) n = pgpt->count - offset;
		memcpy(buf, pgpt->buf + offset, n);
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
	if (pb->start == nil) pb->start = new;
	if (pb->end == nil) pb->end = new;
	else {
		new->prev = pb->end;
		pb->end->next = new;
		pb->end = new;
	}
	pb->size += new->count;
	return new;
}
