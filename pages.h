// managing paginated memory buffers

enum {
	PageSize = 2048,
};

typedef struct Page Page;
typedef struct Array Array;
typedef struct ArHeader ArHeader;
typedef struct ArSlice ArSlice;

struct ArHeader {
	int ref;
	long len;
};

struct Array {
	ArHeader;
	char p[1];
	/* I don't think plan9 C allows
	 * zero-length arrays */
};

struct ArSlice {
	Array *ar;
	char *p;
	long len, cap;
};

struct Page {
	ArSlice *as;
	Page *prev;
	Page *next;
};

typedef struct PBuf PBuf;
struct PBuf {
	Page *start;
	Page *end;
	vlong size; // how many bytes were allocated in total
	vlong length; // how many bytes are used for storage
	//              should be count ≤ size
};

long pbwrite(PBuf *pb, void *buf, long nbytes, vlong offset); 
long pbread(PBuf *pb, void *buf, long nbytes, vlong offset);

Array * allocarray(long);
ArSlice * allocarslice(Array *, long, long);

Page * allocpage(void);
Page * duppage(Page *);
void freepage(Page *);
Page * addpage(PBuf *pb);
