#ifndef _COUNTER_H
#define _COUNTER_H

#define CNTR_TABULATE		0x00000001
#define CNTR_SINGLE_FIELD	0x00000002

#include "burp.h"
#include "cmd.h"
#include "action.h"

#define CNTR_ENT_SIZE	256

#define CNTR_STATUS_STR_SCANNING	"scanning"
#define CNTR_STATUS_STR_BACKUP		"backup"
#define CNTR_STATUS_STR_MERGING		"merging"
#define CNTR_STATUS_STR_SHUFFLING	"shuffling"
#define CNTR_STATUS_STR_LISTING		"listing"
#define CNTR_STATUS_STR_RESTORING	"restoring"
#define CNTR_STATUS_STR_VERIFYING	"verifying"
#define CNTR_STATUS_STR_DELETING	"deleting"
#define CNTR_STATUS_STR_DIFFING		"diffing"

enum cntr_status
{
	CNTR_STATUS_UNSET=0,

	CNTR_STATUS_SCANNING,
	CNTR_STATUS_BACKUP,
	CNTR_STATUS_MERGING,
	CNTR_STATUS_SHUFFLING,

	CNTR_STATUS_LISTING,
	CNTR_STATUS_RESTORING,
	CNTR_STATUS_VERIFYING,
	CNTR_STATUS_DELETING,
	CNTR_STATUS_DIFFING
};

typedef struct cntr_ent cntr_ent_t;

struct cntr_ent
{
	enum cmd cmd;
	char *field;
	char *label;
	uint8_t flags;
	uint64_t count;
	uint64_t changed;
	uint64_t same;
	uint64_t deleted;
	uint64_t phase1;
	cntr_ent_t *next;
};

struct cntr
{
	// Want to be able to order the entries for output.
	struct cntr_ent *list;
	// Also want to be able to index each entry by a cmd, for fast
	// lookup when incrementing a counter.
	struct cntr_ent *ent[CNTR_ENT_SIZE];

	// These should have their own individual cmd entries.
	uint64_t warning;
	uint64_t byte;
	uint64_t recvbyte;
	uint64_t sentbyte;

	// Buffer to use for the forked child to write statuses to the parent.
	size_t str_max_len;
	char *str;

	enum cntr_status cntr_status;

	char *cname;
};

extern struct cntr *cntr_alloc(void);
extern int cntr_init(struct cntr *cntr, const char *cname);
extern void cntr_free(struct cntr **cntr);

extern const char *bytes_to_human(uint64_t counter);
extern void cntr_print(struct cntr *cntr, enum action act);
extern int cntr_stats_to_file(struct cntr *cntr,
	const char *directory, enum action act, struct conf **confs);
extern void cntr_print_end(struct cntr *c);
extern void cntr_print_end_phase1(struct cntr *c);
extern void cntr_add(struct cntr *c, char ch, int print);
extern void cntr_add_same(struct cntr *c, char ch);
extern void cntr_add_changed(struct cntr *c, char ch);
extern void cntr_add_deleted(struct cntr *c, char ch);
extern void cntr_add_bytes(struct cntr *c, uint64_t bytes);
extern void cntr_add_sentbytes(struct cntr *c, uint64_t bytes);
extern void cntr_add_recvbytes(struct cntr *c, uint64_t bytes);

extern void cntr_add_phase1(struct cntr *c,
	char ch, int print);
extern void cntr_add_val(struct cntr *c,
	char ch, uint64_t val, int print);
extern void cntr_add_same_val(struct cntr *c,
	char ch, uint64_t val);
extern void cntr_add_changed_val(struct cntr *c,
	char ch, uint64_t val);

#ifndef HAVE_WIN32
extern size_t cntr_to_str(struct cntr *cntr, const char *path);
extern int cntr_send(struct cntr *cntr);
#endif

extern int str_to_cntr(const char *str, struct cstat *cstat, char **path);
extern int cntr_recv(struct asfd *asfd, struct conf **conf);

extern const char *cntr_status_to_str(struct cntr *cntr);
extern cntr_status cntr_str_to_status(const char *str);

#endif
