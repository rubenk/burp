#ifndef _MANIO_H
#define _MANIO_H

#include <zlib.h>

// Manifests are split up into several files in a directory.
// This is for manipulating them.
// 'manio' means 'manifest I/O'

struct manio
{
	gzFile zp;		// File pointer.
	char *base_dir;		// The base directory. 
	char *directory;	// Directory containing the files.
	uint64_t fcount;	// File name incrementer.
	char *fpath;		// Current file path.
	char *lpath;		// Previous file path.
	char *mode;		// Mode with which to open the files.
	int sig_count;		// When writing, need to split the files
				// after every X signatures written.
	uint8_t first_entry;	// Set to 1 when starting a new manifest
				// component file.
	char *hook_dir;
	char **hook_sort;	// Array for sorting and writing hooks.
	int hook_count;
	char *rdirectory;	// When renaming the manifest directory to its
				// final location, hooks need to be written
				// using the final destination. This is for
				// that circumstance.
	char *dindex_dir;
	char **dindex_sort;	// Array for sorting and writing dindex.
	int dindex_count;
	enum protocol protocol;	// Whether running in protocol1/2 mode.
};

extern struct manio *manio_alloc(void);
extern int manio_free(struct manio **manio);
extern int manio_close(struct manio *manio);
extern int manio_init_read(struct manio *manio, const char *directory);
extern int manio_init_write(struct manio *manio, const char *directory);
extern int manio_set_mode_read(struct manio *manio);
extern int manio_set_mode_write(struct manio *manio);
extern int manio_init_write_hooks(struct manio *manio,
	const char *base_dir, const char *hook_dir, const char *rdirectory);
extern int manio_init_write_dindex(struct manio *manio, const char *dir);
extern void manio_set_protocol(struct manio *manio, enum protocol protocol);
extern int manio_read_fcount(struct manio *manio);

extern int manio_sbuf_fill(struct manio *manio, struct asfd *asfd,
	struct sbuf *sb, struct blk *blk, struct sdirs *sdirs, struct conf **confs);
extern int manio_sbuf_fill_phase1(struct manio *manio, struct asfd *asfd,
	struct sbuf *sb, struct blk *blk, struct sdirs *sdirs, struct conf **confs);

extern int manio_write_sig(struct manio *manio, struct blk *blk);
extern int manio_write_sig_and_path(struct manio *manio, struct blk *blk);
extern int manio_write_sbuf(struct manio *manio, struct sbuf *sb);

extern int manio_closed(struct manio *manio);

extern int manio_copy_entry(struct asfd *asfd,
	struct sbuf **csb, struct sbuf *sb,
	struct blk **blk, struct manio *srcmanio,
	struct manio *dstmanio, struct conf **confs);
extern int manio_forward_through_sigs(struct asfd *asfd, struct sbuf **csb,
	struct blk **blk, struct manio *manio, struct conf **confs);

#endif
