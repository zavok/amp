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
	vlong count; // how many bytes are used for storage
	//              count <= size
};

long pbwrite(PBuf *pb, void *buf, long nbytes, vlong offset); 
long pbread(PBuf *pb, void *buf, long nbytes, vlong offset);

Page * addpage(PBuf *pb);
