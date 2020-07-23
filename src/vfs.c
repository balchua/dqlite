#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <raft.h>

#include <sqlite3.h>

#include "../include/dqlite.h"

#include "lib/assert.h"

#include "format.h"
#include "vfs.h"

#define VFS__V1 1
#define VFS__V2 2

/* Maximum pathname length supported by this VFS. */
#define VFS__MAX_PATHNAME 512

/* Hold content for a shared memory mapping. */
struct vfsShm
{
	void **regions;     /* Pointers to shared memory regions. */
	unsigned n_regions; /* Number of shared memory regions. */

	unsigned shared[SQLITE_SHM_NLOCK];    /* Count of shared locks */
	unsigned exclusive[SQLITE_SHM_NLOCK]; /* Count of exclusive locks */
};

/* Database-specific content */
struct vfsDatabase
{
	unsigned page_size; /* Page size of each page. */
	void **pages;       /* All database. */
	unsigned n_pages;   /* Number of pages. */
	struct vfsShm shm;  /* Shared memory. */
};

/* Hold the content of a single WAL frame. */
struct vfsFrame
{
	uint8_t hdr[FORMAT__WAL_FRAME_HDR_SIZE];
	void *buf; /* Content of the page. */
};

/* WAL-specific content */
struct vfsWal
{
	struct vfsDatabase *database;      /* Associated database */
	uint8_t hdr[FORMAT__WAL_HDR_SIZE]; /* Header. */
	struct vfsFrame **frames;          /* All frames. */
	unsigned n_frames;                 /* Number of frames. */
};

enum vfsContentType {
	VFS__DATABASE, /* Main database file */
	VFS__JOURNAL,  /* Default SQLite journal file */
	VFS__WAL       /* Write-Ahead Log */
};

/* Hold content for a single file in the custom dqlite VFS. */
struct vfsContent
{
	char *filename;           /* Name of the file. */
	enum vfsContentType type; /* Content type (either main db or WAL). */
	unsigned refcount;        /* N. of files referencing this content. */
	union {
		struct vfsDatabase database; /* VFS__DATABASE */
		struct vfsWal wal;           /* VFS__WAL */
	};
};

/* Create a new frame of a WAL file. */
static struct vfsFrame *vfsFrameCreate(int size)
{
	struct vfsFrame *f;

	assert(size > 0);

	f = sqlite3_malloc(sizeof *f);
	if (f == NULL) {
		goto oom;
	}

	f->buf = sqlite3_malloc(size);
	if (f->buf == NULL) {
		goto oom_after_page_alloc;
	}

	memset(f->buf, 0, (size_t)size);
	memset(f->hdr, 0, FORMAT__WAL_FRAME_HDR_SIZE);

	return f;

oom_after_page_alloc:
	sqlite3_free(f);
oom:
	return NULL;
}

/* Destroy a WAL frame */
static void vfsFrameDestroy(struct vfsFrame *f)
{
	assert(f != NULL);
	assert(f->buf != NULL);

	sqlite3_free(f->buf);
	sqlite3_free(f);
}

/* Initialize the shared memory mapping of a database file. */
static void vfsShmInit(struct vfsShm *s)
{
	int i;

	s->regions = NULL;
	s->n_regions = 0;

	for (i = 0; i < SQLITE_SHM_NLOCK; i++) {
		s->shared[i] = 0;
		s->exclusive[i] = 0;
	}
}

/* Release all resources used by a shared memory mapping. */
static void vfsShmClose(struct vfsShm *s)
{
	void *region;
	unsigned i;

	assert(s != NULL);

	/* Free all regions. */
	for (i = 0; i < s->n_regions; i++) {
		region = *(s->regions + i);
		assert(region != NULL);
		sqlite3_free(region);
	}

	/* Free the shared memory region array. */
	if (s->regions != NULL) {
		sqlite3_free(s->regions);
	}
}

/* Revert the shared mamory to its initial state. */
static void vfsShmReset(struct vfsShm *s)
{
	vfsShmClose(s);
	vfsShmInit(s);
}

/* Initialize a new database object. */
static void vfsDatabaseInit(struct vfsDatabase *d)
{
	d->page_size = 0;
	d->pages = NULL;
	d->n_pages = 0;
	vfsShmInit(&d->shm);
}

/* Initialize a new WAL object. */
static void vfsWalInit(struct vfsWal *w)
{
	w->database = NULL;
	memset(w->hdr, 0, FORMAT__WAL_HDR_SIZE);
	w->frames = NULL;
	w->n_frames = 0;
}

/* Create the content structure for a new volatile file. */
static struct vfsContent *vfsContentCreate(const char *name, int type)
{
	struct vfsContent *c;

	assert(name != NULL);
	assert(type == VFS__DATABASE || type == VFS__JOURNAL ||
	       type == VFS__WAL);

	c = sqlite3_malloc(sizeof *c);
	if (c == NULL) {
		goto oom;
	}

	// Copy the name, since when called from Go, the pointer will be freed.
	c->filename = sqlite3_malloc((int)(strlen(name) + 1));
	if (c->filename == NULL) {
		goto oom_after_content_malloc;
	}
	strcpy(c->filename, name);

	c->refcount = 0;
	c->type = type;

	switch (type) {
		case VFS__DATABASE:
			vfsDatabaseInit(&c->database);
			break;
		case VFS__WAL:
			vfsWalInit(&c->wal);
			break;
		case VFS__JOURNAL:
			break;
	}

	return c;

oom_after_content_malloc:
	sqlite3_free(c);
oom:
	return NULL;
}

/* Release all memory used by a database object. */
static void vfsDatabaseClose(struct vfsDatabase *d)
{
	unsigned i;
	for (i = 0; i < d->n_pages; i++) {
		sqlite3_free(d->pages[i]);
	}
	if (d->pages != NULL) {
		sqlite3_free(d->pages);
	}
	vfsShmClose(&d->shm);
}

/* Release all memory used by a WAL object. */
static void vfsWalClose(struct vfsWal *w)
{
	unsigned i;
	for (i = 0; i < w->n_frames; i++) {
		vfsFrameDestroy(w->frames[i]);
	}
	if (w->frames != NULL) {
		sqlite3_free(w->frames);
	}
}

/* Destroy the content of a volatile file. */
static void vfsContentDestroy(struct vfsContent *c)
{
	assert(c != NULL);
	assert(c->filename != NULL);

	/* Free the filename. */
	sqlite3_free(c->filename);

	switch (c->type) {
		case VFS__DATABASE:
			vfsDatabaseClose(&c->database);
			break;
		case VFS__JOURNAL:
			break;
		case VFS__WAL:
			vfsWalClose(&c->wal);
			break;
	}

	sqlite3_free(c);
}

/* Return 1 if this file has no content. */
static int vfsContentIsEmpty(struct vfsContent *c)
{
	assert(c != NULL);

	switch (c->type) {
		case VFS__DATABASE:
			if (c->database.n_pages == 0) {
				assert(c->database.pages == NULL);
				return 1;
			}

			// If it was written, a page list and a page size must
			// have been set.
			assert(c->database.pages != NULL &&
			       c->database.n_pages > 0 &&
			       c->database.page_size > 0);
			break;
		case VFS__WAL:
			if (c->wal.n_frames == 0) {
				assert(c->wal.frames == NULL);
				return 1;
			}

			// If it was written, a page list and a page size must
			// have been set.
			assert(c->wal.frames != NULL && c->wal.n_frames > 0 &&
			       c->wal.database->page_size > 0);
			break;
		case VFS__JOURNAL:
			return 1;
	}

	return 0;
}

/* Get a page from the given database, possibly creating a new one. */
static int vfsDatabasePageGet(struct vfsDatabase *d, int pgno, void **page)
{
	int rc;

	assert(d != NULL);
	assert(pgno > 0);

	/* SQLite should access pages progressively, without jumping more than
	 * one page after the end. */
	if (pgno > (int)(d->n_pages + 1)) {
		rc = SQLITE_IOERR_WRITE;
		goto err;
	}

	if (pgno == (int)(d->n_pages + 1)) {
		/* Create a new page, grow the page array, and append the
		 * new page to it. */
		void **pages; /* New page array. */

		/* We assume that the page size has been set, either by
		 * intercepting the first main database file write, or by
		 * handling a 'PRAGMA page_size=N' command in
		 * vfs__file_control(). This assumption is enforced in
		 * vfsFileWrite(). */
		assert(d->page_size > 0);

		*page = sqlite3_malloc((int)d->page_size);
		if (*page == NULL) {
			rc = SQLITE_NOMEM;
			goto err;
		}

		pages = sqlite3_realloc(d->pages, (int)(sizeof *pages) * pgno);
		if (pages == NULL) {
			rc = SQLITE_NOMEM;
			goto err_after_vfs_page_create;
		}

		/* Append the new page to the new page array. */
		*(pages + pgno - 1) = *page;

		/* Update the page array. */
		d->pages = pages;
		d->n_pages = (unsigned)pgno;
	} else {
		/* Return the existing page. */
		assert(d->pages != NULL);
		*page = d->pages[pgno - 1];
	}

	return SQLITE_OK;

err_after_vfs_page_create:
	sqlite3_free(*page);
err:
	*page = NULL;
	return rc;
}

// Get a frame from the WAL, possibly creating a new one.
static int vfsWalFrameGet(struct vfsWal *w, int pgno, struct vfsFrame **page)
{
	int rc;

	assert(w != NULL);
	assert(pgno > 0);

	/* SQLite should access pages progressively, without jumping more than
	 * one page after the end. */
	if (pgno > (int)(w->n_frames + 1)) {
		rc = SQLITE_IOERR_WRITE;
		goto err;
	}

	if (pgno == (int)(w->n_frames + 1)) {
		/* Create a new page, grow the page array, and append the
		 * new page to it. */
		struct vfsFrame **frames; /* New frames array. */

		/* We assume that the page size has been set, either by
		 * intercepting the first main database file write, or by
		 * handling a 'PRAGMA page_size=N' command in
		 * vfs__file_control(). This assumption is enforced in
		 * vfsFileWrite(). */
		assert(w->database->page_size > 0);

		*page = vfsFrameCreate((int)w->database->page_size);
		if (*page == NULL) {
			rc = SQLITE_NOMEM;
			goto err;
		}

		frames =
		    sqlite3_realloc(w->frames, (int)(sizeof *frames) * pgno);
		if (frames == NULL) {
			rc = SQLITE_NOMEM;
			goto err_after_vfs_page_create;
		}

		/* Append the new page to the new page array. */
		*(frames + pgno - 1) = *page;

		/* Update the page array. */
		w->frames = frames;
		w->n_frames = (unsigned)pgno;
	} else {
		/* Return the existing page. */
		assert(w->frames != NULL);
		*page = w->frames[pgno - 1];
	}

	return SQLITE_OK;

err_after_vfs_page_create:
	vfsFrameDestroy(*page);

err:
	*page = NULL;

	return rc;
}

/* Lookup a page from the given database, returning NULL if it doesn't exist. */
static void *vfsDatabasePageLookup(struct vfsDatabase *d, unsigned pgno)
{
	void *page;

	assert(d != NULL);
	assert(pgno > 0);

	if (pgno > d->n_pages) {
		/* This page hasn't been written yet. */
		return NULL;
	}

	page = d->pages[pgno - 1];

	assert(page != NULL);

	return page;
}

/* Lookup a frame from the WAL, returning NULL if it doesn't exist. */
static struct vfsFrame *vfsWalFrameLookup(struct vfsWal *w, unsigned n)
{
	struct vfsFrame *frame;

	assert(w != NULL);
	assert(n > 0);

	if (n > w->n_frames) {
		/* This page hasn't been written yet. */
		return NULL;
	}

	frame = w->frames[n - 1];

	assert(frame != NULL);

	return frame;
}

/* Truncate the file to be exactly the given number of pages. */
static void vfsContentTruncate(struct vfsContent *content, unsigned n_pages)
{
	void **cursor;
	unsigned i;

	/* We expect callers to only invoke us if some actual content has been
	 * written already. */
	assert(content->database.n_pages > 0);

	/* Truncate should always shrink a file. */
	assert(n_pages <= content->database.n_pages);
	assert(content->database.pages != NULL);

	/* Destroy pages beyond pages_len. */
	cursor = content->database.pages + n_pages;
	for (i = 0; i < (content->database.n_pages - n_pages); i++) {
		sqlite3_free(*cursor);
		cursor++;
	}

	/* Reset the file header (for WAL files). */
	if (content->type == VFS__WAL) {
		/* We expect callers to always truncate the WAL to zero. */
		assert(n_pages == 0);
		assert(content->wal.hdr != NULL);
		memset(content->wal.hdr, 0, FORMAT__WAL_HDR_SIZE);
	}

	/* Shrink the page array, possibly to 0.
	 *
	 * TODO: in principle realloc could fail also when shrinking. */
	content->database.pages =
	    sqlite3_realloc(content->database.pages,
			    (int)(sizeof *(content->database.pages) * n_pages));

	/* Update the page count. */
	content->database.n_pages = n_pages;
}

/* Truncate a WAL file to zero. */
static void vfsWalTruncate(struct vfsContent *content)
{
	unsigned i;

	/* We expect callers to only invoke us if some actual content has been
	 * written already. */
	assert(content->wal.frames != NULL);
	assert(content->wal.n_frames > 0);

	/* Reset the file header (for WAL files). */
	memset(content->wal.hdr, 0, FORMAT__WAL_HDR_SIZE);

	/* Destroy all frames. */
	for (i = 0; i < content->wal.n_frames; i++) {
		vfsFrameDestroy(content->wal.frames[i]);
	}
	sqlite3_free(content->wal.frames);

	content->wal.frames = NULL;
	content->wal.n_frames = 0;
}

/* Implementation of the abstract sqlite3_file base class. */
struct vfsFile
{
	sqlite3_file base;          /* Base class. Must be first. */
	struct vfs *vfs;            /* Pointer to volatile VFS data. */
	struct vfsContent *content; /* Handle to the file content. */
	int flags;                  /* Flags passed to xOpen */
	sqlite3_file *temp;         /* For temp-files, actual VFS. */
};

/* Custom dqlite VFS. Contains pointers to the content of all files that were
 * created. */
struct vfs
{
	struct vfsContent **contents; /* Files content */
	unsigned n_contents;          /* Number of files */
	int error;                    /* Last error occurred. */
	int version;
};

/* Create a new vfs object. */
static struct vfs *vfsCreate(int version)
{
	struct vfs *v;

	v = sqlite3_malloc(sizeof *v);
	if (v == NULL) {
		return NULL;
	}

	v->contents = NULL;
	v->n_contents = 0;
	v->version = version;

	return v;
}

/* Release the memory used internally by the VFS object.
 *
 * All file content will be de-allocated, so dangling open FDs against
 * those files will be broken.
 */
static void vfsDestroy(struct vfs *r)
{
	unsigned i;

	assert(r != NULL);

	for (i = 0; i < r->n_contents; i++) {
		struct vfsContent *content = r->contents[i];
		vfsContentDestroy(content);
	}

	if (r->contents != NULL) {
		sqlite3_free(r->contents);
	}
}

/* Find a content object by filename. */
static struct vfsContent *vfsContentLookup(struct vfs *r, const char *filename)
{
	unsigned i;

	assert(r != NULL);
	assert(filename != NULL);

	for (i = 0; i < r->n_contents; i++) {
		struct vfsContent *content = r->contents[i];
		if (strcmp(content->filename, filename) == 0) {
			// Found matching file.
			return content;
		}
	}

	return NULL;
}

/* Find the database object associated with the given WAL file name. */
static struct vfsDatabase *vfsDatabaseLookup(struct vfs *v,
					     const char *wal_filename)
{
	size_t database_filename_len;
	unsigned i;

	assert(v != NULL);
	assert(wal_filename != NULL);

	database_filename_len = strlen(wal_filename) - strlen("-wal");

	for (i = 0; i < v->n_contents; i++) {
		struct vfsContent *content = v->contents[i];
		if (strncmp(content->filename, wal_filename,
			    database_filename_len) == 0) {
			// Found matching file.
			return &content->database;
		}
	}

	return NULL;
}

static int vfsDeleteContent(struct vfs *r, const char *filename)
{
	unsigned i;

	for (i = 0; i < r->n_contents; i++) {
		struct vfsContent *content = r->contents[i];
		unsigned j;

		if (strcmp(content->filename, filename) != 0) {
			continue;
		}

		/* Check that there are no consumers of this file. */
		if (content->refcount > 0) {
			r->error = EBUSY;
			return SQLITE_IOERR_DELETE;
		}

		/* Free all memory allocated for this file. */
		vfsContentDestroy(content);

		/* Shift all other contents objects. */
		for (j = i + 1; j < r->n_contents; j++) {
			r->contents[j - 1] = r->contents[j];
		}
		r->n_contents--;

		return SQLITE_OK;
	}

	r->error = ENOENT;
	return SQLITE_IOERR_DELETE_NOENT;
}

static int vfsFileClose(sqlite3_file *file)
{
	int rc = SQLITE_OK;
	struct vfsFile *f = (struct vfsFile *)file;
	struct vfs *v = (struct vfs *)(f->vfs);

	if (f->temp != NULL) {
		/* Close the actual temporary file. */
		rc = f->temp->pMethods->xClose(f->temp);
		sqlite3_free(f->temp);

		return rc;
	}

	assert(f->content->refcount);
	f->content->refcount--;

	/* If we got zero references, reset the shared memory mapping. */
	if (f->content->refcount == 0 && f->content->type == VFS__DATABASE) {
		vfsShmReset(&f->content->database.shm);
	}

	if (f->flags & SQLITE_OPEN_DELETEONCLOSE) {
		rc = vfsDeleteContent(v, f->content->filename);
	}

	return rc;
}

/* Read data from the main database. */
static int vfsDatabaseRead(struct vfsDatabase *d,
			   void *buf,
			   int amount,
			   sqlite_int64 offset)
{
	unsigned page_size = d->page_size;
	unsigned pgno;
	void *page;

	if (d->n_pages == 0) {
		return SQLITE_IOERR_SHORT_READ;
	}

	/* If the main database file is not empty, we expect the
	 * page size to have been set by an initial write. */
	assert(page_size > 0);

	if (offset < page_size) {
		/* Reading from page 1. We expect the read to be
		 * at most page_size bytes. */
		assert(amount <= (int)page_size);
		pgno = 1;
	} else {
		/* For pages greater than 1, we expect a full
		 * page read, with an offset that starts exectly
		 * at the page boundary. */
		assert(amount == (int)page_size);
		assert(((unsigned)offset % page_size) == 0);
		pgno = ((unsigned)offset / page_size) + 1;
	}

	assert(pgno > 0);

	page = vfsDatabasePageLookup(d, pgno);

	if (pgno == 1) {
		/* Read the desired part of page 1. */
		memcpy(buf, page + offset, (size_t)amount);
	} else {
		/* Read the full page. */
		memcpy(buf, page, (size_t)amount);
	}

	return SQLITE_OK;
}

/* Read data from the WAL. */
static int vfsWalRead(struct vfsWal *w,
		      void *buf,
		      int amount,
		      sqlite_int64 offset)
{
	unsigned page_size = w->database->page_size;
	unsigned pgno;
	struct vfsFrame *frame;

	if (w->n_frames == 0) {
		return SQLITE_IOERR_SHORT_READ;
	}

	if (offset == 0) {
		/* Read the header. */
		assert(amount == FORMAT__WAL_HDR_SIZE);
		memcpy(buf, w->hdr, FORMAT__WAL_HDR_SIZE);
		return SQLITE_OK;
	}

	/* For any other frame, we expect either a header read,
	 * a checksum read, a page read or a full frame read. */
	if (amount == FORMAT__WAL_FRAME_HDR_SIZE) {
		assert(((offset - FORMAT__WAL_HDR_SIZE) %
			(page_size + FORMAT__WAL_FRAME_HDR_SIZE)) == 0);
		pgno = format__wal_calc_pgno(page_size, (unsigned)offset);
	} else if (amount == sizeof(uint32_t) * 2) {
		if (offset == FORMAT__WAL_FRAME_HDR_SIZE) {
			/* Read the checksum from the WAL
			 * header. */
			memcpy(buf, w->hdr + offset, (size_t)amount);
			return SQLITE_OK;
		}
		assert(((offset - 16 - FORMAT__WAL_HDR_SIZE) %
			(page_size + FORMAT__WAL_FRAME_HDR_SIZE)) == 0);
		pgno = ((unsigned)offset - 16 - FORMAT__WAL_HDR_SIZE) /
			   (page_size + FORMAT__WAL_FRAME_HDR_SIZE) +
		       1;
	} else if (amount == (int)page_size) {
		assert(((offset - FORMAT__WAL_HDR_SIZE -
			 FORMAT__WAL_FRAME_HDR_SIZE) %
			(page_size + FORMAT__WAL_FRAME_HDR_SIZE)) == 0);
		pgno = format__wal_calc_pgno(page_size, (unsigned)offset);
	} else {
		assert(amount == (FORMAT__WAL_FRAME_HDR_SIZE + (int)page_size));
		pgno = format__wal_calc_pgno(page_size, (unsigned)offset);
	}

	if (pgno == 0) {
		// This is an attempt to read a page that was
		// never written.
		memset(buf, 0, (size_t)amount);
		return SQLITE_IOERR_SHORT_READ;
	}

	frame = vfsWalFrameLookup(w, pgno);

	if (amount == FORMAT__WAL_FRAME_HDR_SIZE) {
		memcpy(buf, frame->hdr, (size_t)amount);
	} else if (amount == sizeof(uint32_t) * 2) {
		memcpy(buf, frame->hdr + 16, (size_t)amount);
	} else if (amount == (int)page_size) {
		memcpy(buf, frame->buf, (size_t)amount);
	} else {
		memcpy(buf, frame->hdr, FORMAT__WAL_FRAME_HDR_SIZE);
		memcpy(buf + FORMAT__WAL_FRAME_HDR_SIZE, frame->buf, page_size);
	}

	return SQLITE_OK;
}

static int vfsFileRead(sqlite3_file *file,
		       void *buf,
		       int amount,
		       sqlite_int64 offset)
{
	struct vfsFile *f = (struct vfsFile *)file;
	int rv;

	assert(buf != NULL);
	assert(amount > 0);
	assert(f != NULL);

	if (f->temp != NULL) {
		/* Read from the actual temporary file. */
		return f->temp->pMethods->xRead(f->temp, buf, amount, offset);
	}

	assert(f->content != NULL);
	assert(f->content->filename != NULL);
	assert(f->content->refcount > 0);

	switch (f->content->type) {
		case VFS__DATABASE:
			rv = vfsDatabaseRead(&f->content->database, buf, amount,
					     offset);
			break;
		case VFS__WAL:
			rv = vfsWalRead(&f->content->wal, buf, amount, offset);
			break;
		default:
			rv = SQLITE_IOERR_READ;
			break;
	}

	/* From SQLite docs:
	 *
	 *   If xRead() returns SQLITE_IOERR_SHORT_READ it must also fill
	 *   in the unread portions of the buffer with zeros.  A VFS that
	 *   fails to zero-fill short reads might seem to work.  However,
	 *   failure to zero-fill short reads will eventually lead to
	 *   database corruption.
	 */
	if (rv == SQLITE_IOERR_SHORT_READ) {
		memset(buf, 0, (size_t)amount);
	}

	return rv;
}

static int vfsDatabaseWrite(struct vfsDatabase *d,
			    const void *buf,
			    int amount,
			    sqlite_int64 offset)
{
	unsigned pgno;
	void *page;
	int rc;

	if (offset == 0) {
		unsigned int page_size;

		/* This is the first database page. We expect
		 * the data to contain at least the header. */
		assert(amount >= FORMAT__DB_HDR_SIZE);

		/* Extract the page size from the header. */
		rc = format__get_page_size(FORMAT__DB, buf, &page_size);
		if (rc != SQLITE_OK) {
			return rc;
		}

		if (d->page_size > 0) {
			/* Check that the given page size
			 * actually matches what we have
			 * recorded. Since we make 'PRAGMA
			 * page_size=N' fail if the page is
			 * already set (see struct
			 * vfs__fileControl), there should be
			 * no way for the user to change it. */
			assert(page_size == d->page_size);
		} else {
			/* This must be the very first write to
			 * the
			 * database. Keep track of the page
			 * size. */
			d->page_size = page_size;
		}

		pgno = 1;
	} else {
		/* The header must have been written and the
		 * page size set. */
		if (d->page_size == 0) {
			return SQLITE_IOERR_WRITE;
		}

		/* For pages beyond the first we expect offset
		 * to be a multiple of the page size. */
		assert((offset % d->page_size) == 0);

		/* We expect that SQLite writes a page at time.
		 */
		assert(amount == (int)d->page_size);

		pgno = ((unsigned)offset / d->page_size) + 1;
	}

	rc = vfsDatabasePageGet(d, (int)pgno, &page);
	if (rc != SQLITE_OK) {
		return rc;
	}

	assert(page != NULL);

	memcpy(page, buf, (size_t)amount);

	return SQLITE_OK;
}

static int vfsWalWrite(struct vfsWal *w,
		       const void *buf,
		       int amount,
		       sqlite_int64 offset)
{
	unsigned page_size = w->database->page_size;
	unsigned pgno;
	struct vfsFrame *frame;

	if (offset == 0) {
		/* This is the WAL header. */
		unsigned int new_page_size;
		int rc;

		/* We expect the data to contain exactly 32
		 * bytes. */
		assert(amount == FORMAT__WAL_HDR_SIZE);

		/* The page size indicated in the header must be
		 * valid
		 * and match the one of the database file. */
		rc = format__get_page_size(FORMAT__WAL, buf, &new_page_size);
		if (rc != SQLITE_OK) {
			return SQLITE_CORRUPT;
		}

		if (new_page_size != page_size) {
			return SQLITE_CORRUPT;
		}

		memcpy(w->hdr, buf, (size_t)amount);
		return SQLITE_OK;
	}

	assert(page_size > 0);

	/* This is a WAL frame write. We expect either a frame
	 * header or page write. */
	if (amount == FORMAT__WAL_FRAME_HDR_SIZE) {
		/* Frame header write. */
		assert(((offset - FORMAT__WAL_HDR_SIZE) %
			(page_size + FORMAT__WAL_FRAME_HDR_SIZE)) == 0);

		pgno = format__wal_calc_pgno(page_size, (unsigned)offset);

		vfsWalFrameGet(w, (int)pgno, &frame);
		if (frame == NULL) {
			return SQLITE_NOMEM;
		}
		memcpy(frame->hdr, buf, (size_t)amount);
	} else {
		/* Frame page write. */
		assert(amount == (int)page_size);
		assert(((offset - FORMAT__WAL_HDR_SIZE -
			 FORMAT__WAL_FRAME_HDR_SIZE) %
			(page_size + FORMAT__WAL_FRAME_HDR_SIZE)) == 0);

		pgno = format__wal_calc_pgno(page_size, (unsigned)offset);

		/* The header for the this frame must already
		 * have been written, so the page is there. */
		frame = vfsWalFrameLookup(w, pgno);

		assert(frame != NULL);

		memcpy(frame->buf, buf, (size_t)amount);
	}

	return SQLITE_OK;
}

static int vfsFileWrite(sqlite3_file *file,
			const void *buf,
			int amount,
			sqlite_int64 offset)
{
	struct vfsFile *f = (struct vfsFile *)file;
	int rv;

	assert(buf != NULL);
	assert(amount > 0);
	assert(f != NULL);

	if (f->temp != NULL) {
		/* Write to the actual temporary file. */
		return f->temp->pMethods->xWrite(f->temp, buf, amount, offset);
	}

	assert(f->content != NULL);
	assert(f->content->filename != NULL);
	assert(f->content->refcount > 0);

	switch (f->content->type) {
		case VFS__DATABASE:
			rv = vfsDatabaseWrite(&f->content->database, buf,
					      amount, offset);
			break;
		case VFS__WAL:
			rv = vfsWalWrite(&f->content->wal, buf, amount, offset);
			break;
		case VFS__JOURNAL:
			/* Silently swallow writes to the journal */
			rv = SQLITE_OK;
			break;
		default:
			rv = SQLITE_IOERR_WRITE;
			break;
	}

	return rv;
}

static int vfsFileTruncate(sqlite3_file *file, sqlite_int64 size)
{
	struct vfsFile *f = (struct vfsFile *)file;
	int pgno;

	assert(f != NULL);
	assert(f->content != NULL);

	/* We expect calls to xTruncate only for database and WAL files. */
	if (f->content->type != VFS__DATABASE && f->content->type != VFS__WAL) {
		return SQLITE_IOERR_TRUNCATE;
	}

	/* Check if this file empty.*/
	if (vfsContentIsEmpty(f->content)) {
		if (size > 0) {
			return SQLITE_IOERR_TRUNCATE;
		}

		/* Nothing to do. */
		return SQLITE_OK;
	}

	switch (f->content->type) {
		case VFS__DATABASE:
			/* Main database. */

			/* Since the file size is not zero, some content must
			 * have been written and the page size must be known. */
			assert(f->content->database.page_size > 0);

			if ((size % f->content->database.page_size) != 0) {
				return SQLITE_IOERR_TRUNCATE;
			}

			pgno = (int)(size / f->content->database.page_size);
			vfsContentTruncate(f->content, (unsigned)pgno);
			break;

		case VFS__WAL:
			/* WAL file. */

			/* We expect SQLite to only truncate to zero, after a
			 * full checkpoint.
			 *
			 * TODO: figure out other case where SQLite might
			 * truncate to a different size.
			 */
			if (size != 0) {
				return SQLITE_PROTOCOL;
			}
			vfsWalTruncate(f->content);
			break;

		default:
			return SQLITE_IOERR_TRUNCATE;
	}

	return SQLITE_OK;
}

static int vfsFileSync(sqlite3_file *file, int flags)
{
	(void)file;
	(void)flags;

	return SQLITE_IOERR_FSYNC;
}

static int vfsFileSize(sqlite3_file *file, sqlite_int64 *size)
{
	struct vfsFile *f = (struct vfsFile *)file;

	switch (f->content->type) {
		case VFS__DATABASE:
			*size = f->content->database.n_pages *
				f->content->database.page_size;
			break;

		case VFS__JOURNAL:
			*size = 0;
			break;

		case VFS__WAL:
			/* TODO? here we assume that FileSize() is never invoked
			 * between a header write and a page write. */
			*size = (f->content->wal.n_frames *
				 (FORMAT__WAL_FRAME_HDR_SIZE +
				  f->content->wal.database->page_size));
			if (*size > 0) {
				*size += FORMAT__WAL_HDR_SIZE;
			}
			break;
	}

	return SQLITE_OK;
}

/* Locking a file is a no-op, since no other process has visibility on it. */
static int vfsFileLock(sqlite3_file *file, int lock)
{
	(void)file;
	(void)lock;

	return SQLITE_OK;
}

/* Unlocking a file is a no-op, since no other process has visibility on it. */
static int vfsFileUnlock(sqlite3_file *file, int lock)
{
	(void)file;
	(void)lock;

	return SQLITE_OK;
}

/* We always report that a lock is held. This routine should be used only in
 * journal mode, so it doesn't matter. */
static int vfsFileCheckReservedLock(sqlite3_file *file, int *result)
{
	(void)file;

	*result = 1;
	return SQLITE_OK;
}

/* Handle pragma a pragma file control. See the xFileControl
 * docstring in sqlite.h.in for more details. */
static int vfsFileControlPragma(struct vfsFile *f, char **fnctl)
{
	const char *left;
	const char *right;

	assert(f != NULL);
	assert(f->content->type == VFS__DATABASE);
	assert(fnctl != NULL);

	left = fnctl[1];
	right = fnctl[2];

	assert(left != NULL);

	if (strcmp(left, "page_size") == 0 && right) {
		/* When the user executes 'PRAGMA page_size=N' we save the
		 * size internally.
		 *
		 * The page size must be between 512 and 65536, and be a
		 * power of two. The check below was copied from
		 * sqlite3BtreeSetPageSize in btree.c.
		 *
		 * Invalid sizes are simply ignored, SQLite will do the same.
		 *
		 * It's not possible to change the size after it's set.
		 */
		int page_size = atoi(right);

		if (page_size >= FORMAT__PAGE_SIZE_MIN &&
		    page_size <= FORMAT__PAGE_SIZE_MAX &&
		    ((page_size - 1) & page_size) == 0) {
			if (f->content->database.page_size &&
			    page_size != (int)f->content->database.page_size) {
				fnctl[0] =
				    "changing page size is not supported";
				return SQLITE_IOERR;
			}
			f->content->database.page_size = (unsigned)page_size;
		}
	} else if (strcmp(left, "journal_mode") == 0 && right) {
		/* When the user executes 'PRAGMA journal_mode=x' we ensure
		 * that the desired mode is 'wal'. */
		if (strcasecmp(right, "wal") != 0) {
			fnctl[0] = "only WAL mode is supported";
			return SQLITE_IOERR;
		}
	}

	/* We're returning NOTFOUND here to tell SQLite that we wish it to go on
	 * with its own handling as well. If we returned SQLITE_OK the page size
	 * of the journal mode wouldn't be effectively set, as the processing of
	 * the PRAGMA would stop here. */
	return SQLITE_NOTFOUND;
}

static int vfsFileControl(sqlite3_file *file, int op, void *arg)
{
	struct vfsFile *f = (struct vfsFile *)file;

	switch (op) {
		case SQLITE_FCNTL_PRAGMA:
			return vfsFileControlPragma(f, arg);
	}

	return SQLITE_OK;
}

static int vfsFileSectorSize(sqlite3_file *file)
{
	(void)file;

	return 0;
}

static int vfsFileDeviceCharacteristics(sqlite3_file *file)
{
	(void)file;

	return 0;
}

static int vfsShmMap(struct vfsShm *s,
		     unsigned region_index,
		     unsigned region_size,
		     bool extend,
		     void volatile **out)
{
	void *region;
	int rv;

	if (s->regions != NULL && region_index < s->n_regions) {
		/* The region was already allocated. */
		region = s->regions[region_index];
		assert(region != NULL);
	} else {
		if (extend) {
			/* We should grow the map one region at a time. */
			assert(region_index == s->n_regions);
			region = sqlite3_malloc64(region_size);
			if (region == NULL) {
				rv = SQLITE_NOMEM;
				goto err;
			}

			memset(region, 0, region_size);

			s->regions = sqlite3_realloc64(
			    s->regions,
			    sizeof *s->regions * (region_index + 1));

			if (s->regions == NULL) {
				rv = SQLITE_NOMEM;
				goto err_after_region_malloc;
			}

			s->regions[region_index] = region;
			s->n_regions++;

		} else {
			/* The region was not allocated and we don't have to
			 * extend the map. */
			region = NULL;
		}
	}

	*out = region;

	return SQLITE_OK;

err_after_region_malloc:
	sqlite3_free(region);
err:
	assert(rv != SQLITE_OK);
	*out = NULL;
	return rv;
}

/* Simulate shared memory by allocating on the C heap. */
static int vfsFileShmMap(sqlite3_file *file, /* Handle open on database file */
			 int region_index,   /* Region to retrieve */
			 int region_size,    /* Size of regions */
			 int extend, /* True to extend file if necessary */
			 void volatile **out /* OUT: Mapped memory */
)
{
	struct vfsFile *f = (struct vfsFile *)file;

	assert(f->content->type == VFS__DATABASE);

	return vfsShmMap(&f->content->database.shm, (unsigned)region_index,
			 (unsigned)region_size, extend, out);
}

static int vfsShmLock(struct vfsShm *s, int ofst, int n, int flags)
{
	int i;

	if (flags & SQLITE_SHM_EXCLUSIVE) {
		/* No shared or exclusive lock must be held in the region. */
		for (i = ofst; i < ofst + n; i++) {
			if (s->shared[i] > 0 || s->exclusive[i] > 0) {
				return SQLITE_BUSY;
			}
		}

		for (i = ofst; i < ofst + n; i++) {
			assert(s->exclusive[i] == 0);
			s->exclusive[i] = 1;
		}
	} else {
		/* No exclusive lock must be held in the region. */
		for (i = ofst; i < ofst + n; i++) {
			if (s->exclusive[i] > 0) {
				return SQLITE_BUSY;
			}
		}

		for (i = ofst; i < ofst + n; i++) {
			s->shared[i]++;
		}
	}

	return SQLITE_OK;
}

static int vfsShmUnlock(struct vfsShm *s, int ofst, int n, int flags)
{
	unsigned *these_locks;
	unsigned *other_locks;
	int i;

	if (flags & SQLITE_SHM_SHARED) {
		these_locks = s->shared;
		other_locks = s->exclusive;
	} else {
		these_locks = s->exclusive;
		other_locks = s->shared;
	}

	for (i = ofst; i < ofst + n; i++) {
		/* Sanity check that no lock of the other type is held in this
		 * region. */
		assert(other_locks[i] == 0);

		/* Only decrease the lock count if it's positive. In other words
		 * releasing a never acquired lock is legal and idemponent. */
		if (these_locks[i] > 0) {
			these_locks[i]--;
		}
	}

	return SQLITE_OK;
}

static int vfsFileShmLock(sqlite3_file *file, int ofst, int n, int flags)
{
	struct vfsFile *f;
	struct vfsShm *shm;
	int rv;

	assert(file != NULL);
	assert(ofst >= 0);
	assert(n >= 0);

	/* Legal values for the offset and the range */
	assert(ofst >= 0 && ofst + n <= SQLITE_SHM_NLOCK);
	assert(n >= 1);
	assert(n == 1 || (flags & SQLITE_SHM_EXCLUSIVE) != 0);

	/* Legal values for the flags.
	 *
	 * See https://sqlite.org/c3ref/c_shm_exclusive.html. */
	assert(flags == (SQLITE_SHM_LOCK | SQLITE_SHM_SHARED) ||
	       flags == (SQLITE_SHM_LOCK | SQLITE_SHM_EXCLUSIVE) ||
	       flags == (SQLITE_SHM_UNLOCK | SQLITE_SHM_SHARED) ||
	       flags == (SQLITE_SHM_UNLOCK | SQLITE_SHM_EXCLUSIVE));

	/* This is a no-op since shared-memory locking is relevant only for
	 * inter-process concurrency. See also the unix-excl branch from
	 * upstream (git commit cda6b3249167a54a0cf892f949d52760ee557129). */

	f = (struct vfsFile *)file;

	assert(f->content != NULL);

	shm = &f->content->database.shm;
	if (flags & SQLITE_SHM_UNLOCK) {
		rv = vfsShmUnlock(shm, ofst, n, flags);
	} else {
		rv = vfsShmLock(shm, ofst, n, flags);
	}

	return rv;
}

static void vfsFileShmBarrier(sqlite3_file *file)
{
	(void)file;
	/* This is a no-op since we expect SQLite to be compiled with mutex
	 * support (i.e. SQLITE_MUTEX_OMIT or SQLITE_MUTEX_NOOP are *not*
	 * defined, see sqliteInt.h). */
}

static int vfsFileShmUnmap(sqlite3_file *file, int delete_flag)
{
	(void)file;
	(void)delete_flag;
	return SQLITE_OK;
}

static const sqlite3_io_methods vfsFileMethods = {
    2,                             // iVersion
    vfsFileClose,                  // xClose
    vfsFileRead,                   // xRead
    vfsFileWrite,                  // xWrite
    vfsFileTruncate,               // xTruncate
    vfsFileSync,                   // xSync
    vfsFileSize,                   // xFileSize
    vfsFileLock,                   // xLock
    vfsFileUnlock,                 // xUnlock
    vfsFileCheckReservedLock,      // xCheckReservedLock
    vfsFileControl,                // xFileControl
    vfsFileSectorSize,             // xSectorSize
    vfsFileDeviceCharacteristics,  // xDeviceCharacteristics
    vfsFileShmMap,                 // xShmMap
    vfsFileShmLock,                // xShmLock
    vfsFileShmBarrier,             // xShmBarrier
    vfsFileShmUnmap,               // xShmUnmap
    0,
    0,
};

static int vfsOpen(sqlite3_vfs *vfs,
		   const char *filename,
		   sqlite3_file *file,
		   int flags,
		   int *out_flags)
{
	struct vfs *v;
	struct vfsFile *f;
	struct vfsContent *content;
	enum vfsContentType type;
	bool exists;
	int exclusive = flags & SQLITE_OPEN_EXCLUSIVE;
	int create = flags & SQLITE_OPEN_CREATE;
	int rc;

	(void)out_flags;

	assert(vfs != NULL);
	assert(vfs->pAppData != NULL);
	assert(file != NULL);

	/* From sqlite3.h.in:
	 *
	 *   The SQLITE_OPEN_EXCLUSIVE flag is always used in conjunction with
	 *   the SQLITE_OPEN_CREATE flag, which are both directly analogous to
	 *   the O_EXCL and O_CREAT flags of the POSIX open() API.  The
	 *   SQLITE_OPEN_EXCLUSIVE flag, when paired with the
	 *   SQLITE_OPEN_CREATE, is used to indicate that file should always be
	 *   created, and that it is an error if it already exists.  It is not
	 *   used to indicate the file should be opened for exclusive access.
	 */
	assert(!exclusive || create);

	v = (struct vfs *)(vfs->pAppData);
	f = (struct vfsFile *)file;

	/* This tells SQLite to not call Close() in case we return an error. */
	f->base.pMethods = 0;
	f->temp = NULL;

	/* Save the flags */
	f->flags = flags;

	/* From SQLite documentation:
	 *
	 * If the zFilename parameter to xOpen is a NULL pointer then xOpen
	 * must invent its own temporary name for the file. Whenever the
	 * xFilename parameter is NULL it will also be the case that the
	 * flags parameter will include SQLITE_OPEN_DELETEONCLOSE.
	 */
	if (filename == NULL) {
		assert(flags & SQLITE_OPEN_DELETEONCLOSE);

		/* Open an actual temporary file. */
		vfs = sqlite3_vfs_find("unix");
		assert(vfs != NULL);

		f->temp = sqlite3_malloc(vfs->szOsFile);
		if (f->temp == NULL) {
			v->error = ENOENT;
			return SQLITE_CANTOPEN;
		}
		rc = vfs->xOpen(vfs, NULL, f->temp, flags, out_flags);
		if (rc != SQLITE_OK) {
			sqlite3_free(f->temp);
			return rc;
		}

		f->base.pMethods = &vfsFileMethods;
		f->vfs = NULL;
		f->content = NULL;

		return SQLITE_OK;
	}

	/* Search if the file exists already. */
	content = vfsContentLookup(v, filename);
	exists = content != NULL;

	/* If file exists, and the exclusive flag is on, return an error. */
	if (exists && exclusive && create) {
		v->error = EEXIST;
		rc = SQLITE_CANTOPEN;
		goto err;
	}

	if (!exists) {
		struct vfsContent **contents;
		unsigned n = v->n_contents + 1;

		/* Check the create flag. */
		if (!create) {
			v->error = ENOENT;
			rc = SQLITE_CANTOPEN;
			goto err;
		}

		if (flags & SQLITE_OPEN_MAIN_DB) {
			type = VFS__DATABASE;
		} else if (flags & SQLITE_OPEN_MAIN_JOURNAL) {
			type = VFS__JOURNAL;
		} else if (flags & SQLITE_OPEN_WAL) {
			type = VFS__WAL;
		} else {
			v->error = ENOENT;
			return SQLITE_CANTOPEN;
		}

		/* Create a new entry. */
		contents = sqlite3_realloc64(v->contents, sizeof *contents * n);
		if (contents == NULL) {
			v->error = ENOMEM;
			rc = SQLITE_CANTOPEN;
			goto err;
		}
		v->contents = contents;

		content = vfsContentCreate(filename, type);
		if (content == NULL) {
			v->error = ENOMEM;
			rc = SQLITE_NOMEM;
			goto err;
		}

		if (type == VFS__WAL) {
			struct vfsDatabase *database;
			/* An associated database file must have been
			 * opened. */
			database = vfsDatabaseLookup(v, filename);
			if (database == NULL) {
				rc = SQLITE_CANTOPEN;
				goto err_after_content_create;
			}
			content->wal.database = database;
		}

		v->contents[n - 1] = content;
		v->n_contents = n;
	}

	// Populate the new file handle.
	f->base.pMethods = &vfsFileMethods;
	f->vfs = v;
	f->content = content;

	content->refcount++;

	return SQLITE_OK;

err_after_content_create:
	vfsContentDestroy(content);
err:
	assert(rc != SQLITE_OK);
	return rc;
}

static int vfsDelete(sqlite3_vfs *vfs, const char *filename, int dir_sync)
{
	struct vfs *v;

	(void)dir_sync;

	assert(vfs != NULL);
	assert(vfs->pAppData != NULL);

	v = (struct vfs *)(vfs->pAppData);

	return vfsDeleteContent(v, filename);
}

static int vfsAccess(sqlite3_vfs *vfs,
		     const char *filename,
		     int flags,
		     int *result)
{
	struct vfs *v;
	struct vfsContent *content;

	(void)flags;

	assert(vfs != NULL);
	assert(vfs->pAppData != NULL);

	v = (struct vfs *)(vfs->pAppData);

	/* If the file exists, access is always granted. */
	content = vfsContentLookup(v, filename);
	if (content == NULL) {
		*result = 0;
	} else {
		*result = 1;
	}

	return SQLITE_OK;
}

static int vfsFullPathname(sqlite3_vfs *vfs,
			   const char *filename,
			   int pathname_len,
			   char *pathname)
{
	(void)vfs;

	// Just return the path unchanged.
	sqlite3_snprintf(pathname_len, pathname, "%s", filename);
	return SQLITE_OK;
}

static void *vfsDlOpen(sqlite3_vfs *vfs, const char *filename)
{
	(void)vfs;
	(void)filename;

	return 0;
}

static void vfsDlError(sqlite3_vfs *vfs, int nByte, char *zErrMsg)
{
	(void)vfs;

	sqlite3_snprintf(nByte, zErrMsg,
			 "Loadable extensions are not supported");
	zErrMsg[nByte - 1] = '\0';
}

static void (*vfsDlSym(sqlite3_vfs *vfs, void *pH, const char *z))(void)
{
	(void)vfs;
	(void)pH;
	(void)z;

	return 0;
}

static void vfsDlClose(sqlite3_vfs *vfs, void *pHandle)
{
	(void)vfs;
	(void)pHandle;

	return;
}

static int vfsRandomness(sqlite3_vfs *vfs, int nByte, char *zByte)
{
	(void)vfs;
	(void)nByte;
	(void)zByte;

	/* TODO (is this needed?) */
	return SQLITE_OK;
}

static int vfsSleep(sqlite3_vfs *vfs, int microseconds)
{
	(void)vfs;

	/* TODO (is this needed?) */
	return microseconds;
}

static int vfsCurrentTimeInt64(sqlite3_vfs *vfs, sqlite3_int64 *piNow)
{
	static const sqlite3_int64 unixEpoch =
	    24405875 * (sqlite3_int64)8640000;
	struct timeval now;

	(void)vfs;

	gettimeofday(&now, 0);
	*piNow =
	    unixEpoch + 1000 * (sqlite3_int64)now.tv_sec + now.tv_usec / 1000;
	return SQLITE_OK;
}

static int vfsCurrentTime(sqlite3_vfs *vfs, double *piNow)
{
	// TODO: check if it's always safe to cast a double* to a
	// sqlite3_int64*.
	return vfsCurrentTimeInt64(vfs, (sqlite3_int64 *)piNow);
}

static int vfsGetLastError(sqlite3_vfs *vfs, int x, char *y)
{
	struct vfs *v = (struct vfs *)(vfs->pAppData);
	int rc;

	(void)vfs;
	(void)x;
	(void)y;

	rc = v->error;

	return rc;
}

static int vfsInit(struct sqlite3_vfs *vfs, const char *name, int version)
{
	vfs->iVersion = 2;
	vfs->szOsFile = sizeof(struct vfsFile);
	vfs->mxPathname = VFS__MAX_PATHNAME;
	vfs->pNext = NULL;

	vfs->pAppData = vfsCreate(version);
	if (vfs->pAppData == NULL) {
		return DQLITE_NOMEM;
	}

	vfs->xOpen = vfsOpen;
	vfs->xDelete = vfsDelete;
	vfs->xAccess = vfsAccess;
	vfs->xFullPathname = vfsFullPathname;
	vfs->xDlOpen = vfsDlOpen;
	vfs->xDlError = vfsDlError;
	vfs->xDlSym = vfsDlSym;
	vfs->xDlClose = vfsDlClose;
	vfs->xRandomness = vfsRandomness;
	vfs->xSleep = vfsSleep;
	vfs->xCurrentTime = vfsCurrentTime;
	vfs->xGetLastError = vfsGetLastError;
	vfs->xCurrentTimeInt64 = vfsCurrentTimeInt64;
	vfs->zName = name;

	return 0;
}

int VfsInitV1(struct sqlite3_vfs *vfs, const char *name)
{
	int rv;

	rv = vfsInit(vfs, name, VFS__V1);
	if (rv != 0) {
		return rv;
	}

	sqlite3_vfs_register(vfs, 0);

	return 0;
}

int VfsInitV2(struct sqlite3_vfs *vfs, const char *name)
{
	return vfsInit(vfs, name, VFS__V2);
}

void VfsClose(struct sqlite3_vfs *vfs)
{
	struct vfs *v = vfs->pAppData;
	if (v->version == VFS__V1) {
		sqlite3_vfs_unregister(vfs);
	}
	vfsDestroy(v);
	sqlite3_free(v);
}

/* Guess the file type by looking the filename. */
static int vfsGuessFileType(const char *filename)
{
	/* TODO: improve the check. */
	if (strstr(filename, "-wal") != NULL) {
		return FORMAT__WAL;
	}

	return FORMAT__DB;
}

int VfsFileRead(const char *vfs_name,
		const char *filename,
		void **buf,
		size_t *len)
{
	sqlite3_vfs *vfs;
	int type;
	int flags;
	sqlite3_file *file;
	sqlite3_int64 file_size;
	unsigned page_size;
	sqlite3_int64 offset;
	int rc;

	assert(vfs_name != NULL);
	assert(filename != NULL);
	assert(buf != NULL);
	assert(len != NULL);

	/* Lookup the VFS object to use. */
	vfs = sqlite3_vfs_find(vfs_name);
	if (vfs == NULL) {
		rc = SQLITE_ERROR;
		goto err;
	}

	type = vfsGuessFileType(filename);

	/* Common flags */
	flags = SQLITE_OPEN_READWRITE;

	if (type == FORMAT__DB) {
		flags |= SQLITE_OPEN_MAIN_DB;
	} else {
		flags |= SQLITE_OPEN_WAL;
	}

	/* Open the file */
	file = sqlite3_malloc(vfs->szOsFile);
	if (file == NULL) {
		rc = SQLITE_NOMEM;
		goto err;
	}

	rc = vfs->xOpen(vfs, filename, file, flags, &flags);
	if (rc != SQLITE_OK) {
		goto err_after_file_malloc;
	}

	/* Get the file size */
	rc = file->pMethods->xFileSize(file, &file_size);
	if (rc != SQLITE_OK) {
		goto err_after_file_open;
	}
	*len = (size_t)file_size;

	/* Check if the file is empty. */
	if (*len == 0) {
		*buf = NULL;
		goto out;
	}

	/* Allocate the read buffer.
	 *
	 * TODO: we should fix the tests and use sqlite3_malloc instead. */
	*buf = raft_malloc(*len);
	if (*buf == NULL) {
		rc = SQLITE_NOMEM;
		goto err_after_file_open;
	}

	/* Read the header. The buffer size is enough for both database and WAL
	 * files. */
	rc = file->pMethods->xRead(file, *buf, FORMAT__WAL_HDR_SIZE, 0);
	if (rc != SQLITE_OK) {
		goto err_after_buf_malloc;
	}

	/* Figure the page size. */
	rc = format__get_page_size(type, *buf, &page_size);
	if (rc != SQLITE_OK) {
		goto err_after_buf_malloc;
	}

	offset = 0;

	/* If this is a WAL file , we have already read the header and we can
	 * move on. */
	if (type == FORMAT__WAL) {
		offset += FORMAT__WAL_HDR_SIZE;
	}

	while ((size_t)offset < *len) {
		uint8_t *pos = (*buf) + offset;

		if (type == FORMAT__WAL) {
			/* Read the frame header */
			rc = file->pMethods->xRead(
			    file, pos, FORMAT__WAL_FRAME_HDR_SIZE, offset);
			if (rc != SQLITE_OK) {
				goto err_after_buf_malloc;
			}
			offset += FORMAT__WAL_FRAME_HDR_SIZE;
			pos += FORMAT__WAL_FRAME_HDR_SIZE;
		}

		/* Read the page */
		rc = file->pMethods->xRead(file, pos, (int)page_size, offset);
		if (rc != SQLITE_OK) {
			goto err_after_buf_malloc;
		}
		offset += page_size;
	};

out:
	file->pMethods->xClose(file);
	sqlite3_free(file);

	return SQLITE_OK;

err_after_buf_malloc:
	sqlite3_free(*buf);

err_after_file_open:
	file->pMethods->xClose(file);

err_after_file_malloc:
	sqlite3_free(file);

err:
	assert(rc != SQLITE_OK);

	*buf = NULL;
	*len = 0;

	return rc;
}

int VfsFileWrite(const char *vfs_name,
		 const char *filename,
		 const void *buf,
		 size_t len)
{
	sqlite3_vfs *vfs;
	sqlite3_file *file;
	int type;
	int flags;
	unsigned int page_size;
	sqlite3_int64 offset;
	const uint8_t *pos;
	int rc;

	assert(vfs_name != NULL);
	assert(filename != NULL);
	assert(buf != NULL);
	assert(len > 0);

	/* Lookup the VFS object to use. */
	vfs = sqlite3_vfs_find(vfs_name);
	if (vfs == NULL) {
		rc = SQLITE_ERROR;
		goto err;
	}

	/* Determine if this is a database or a WAL file. */
	type = vfsGuessFileType(filename);

	/* Common flags */
	flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;

	if (type == FORMAT__DB) {
		flags |= SQLITE_OPEN_MAIN_DB;
	} else {
		flags |= SQLITE_OPEN_WAL;
	}

	/* Open the file */
	file = (sqlite3_file *)sqlite3_malloc(vfs->szOsFile);
	if (file == NULL) {
		rc = SQLITE_NOMEM;
		goto err;
	}
	rc = vfs->xOpen(vfs, filename, file, flags, &flags);
	if (rc != SQLITE_OK) {
		goto err_after_file_malloc;
	}

	/* Truncate any existing content. */
	rc = file->pMethods->xTruncate(file, 0);
	if (rc != SQLITE_OK) {
		goto err_after_file_malloc;
	}

	/* Figure out the page size */
	rc = format__get_page_size(type, buf, &page_size);
	if (rc != SQLITE_OK) {
		goto err_after_file_open;
	}

	offset = 0;
	pos = buf;

	/* If this is a WAL file , write the header first. */
	if (type == FORMAT__WAL) {
		rc = file->pMethods->xWrite(file, pos, FORMAT__WAL_HDR_SIZE,
					    offset);
		if (rc != SQLITE_OK) {
			goto err_after_file_open;
		}
		offset += FORMAT__WAL_HDR_SIZE;
		pos += FORMAT__WAL_HDR_SIZE;
	}

	while ((size_t)offset < len) {
		if (type == FORMAT__WAL) {
			/* Write the frame header */
			rc = file->pMethods->xWrite(
			    file, pos, FORMAT__WAL_FRAME_HDR_SIZE, offset);
			if (rc != SQLITE_OK) {
				goto err_after_file_open;
			}
			offset += FORMAT__WAL_FRAME_HDR_SIZE;
			pos += FORMAT__WAL_FRAME_HDR_SIZE;
		}

		/* Write the page */
		rc = file->pMethods->xWrite(file, pos, (int)page_size, offset);
		if (rc != SQLITE_OK) {
			goto err_after_file_open;
		}
		offset += page_size;
		pos += page_size;
	};

	file->pMethods->xClose(file);
	sqlite3_free(file);

	return SQLITE_OK;

err_after_file_open:
	file->pMethods->xClose(file);

err_after_file_malloc:
	sqlite3_free(file);

err:
	assert(rc != SQLITE_OK);

	return rc;
}
