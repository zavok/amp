// managing paginated memory buffers

enum {
	PageSize = 2048,
};

typedef struct Page Page;
struct Page {
	Page *prev;
	Page *next;
	vlong count;
	char *buf;
};

Page *allocpage(void);

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

Page * addpage(PBuf *pb);
