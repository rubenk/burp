#ifndef _ASYNCIO_ROUTINES_H
#define _ASYNCIO_ROUTINES_H

#define ASYNC_BUF_LEN	16000
#define ZCHUNK		ASYNC_BUF_LEN

#include <zlib.h>
#include "cmd.h"
#include "ssl.h"

extern int status_wfd; // for the child to send information to the parent.
extern int status_rfd; // for the child to read information from the parent.

extern size_t writebuflen;

struct iobuf
{
	char cmd;
	char *buf;
	size_t len;
};

extern struct iobuf *iobuf_alloc(void);
extern void iobuf_free(struct iobuf *iobuf);

extern int async_init(int afd, SSL *assl, struct config *conf, int estimate);

extern void async_free(void);

extern int async_get_fd(void);

extern int async_append_all_to_write_buffer(char wcmd, const char *wsrc, size_t *wlen);

// This one can return without completing the read or write, so check
// *rdst and/or wlen.
extern int async_rw(char *rcmd, char **rdst, size_t *rlen,
        char wcmd, const char *wsrc, size_t *wlen);
extern int async_rw_ng(struct iobuf *rbuf, struct iobuf *wbuf);

extern int async_read_quick(char *rcmd, char **rdst, size_t *rlen);

extern int async_read(char *rcmd, char **rdst, size_t *rlen);
extern int async_read_ng(struct iobuf *rbuf);

extern int async_write(char wcmd, const char *wsrc, size_t wlen);
extern int async_write_ng(struct iobuf *wbuf);

extern int async_write_str(char wcmd, const char *wsrc);

extern int async_read_expect(char cmd, const char *expect);

extern void log_and_send(const char *msg);
extern void log_and_send_oom(const char *function);

// for debug purposes
extern void settimers(int sec, int usec);

// should be in src/lib/log.c
int logw(struct cntr *cntr, const char *fmt, ...);

extern int set_bulk_packets(void);

#endif
