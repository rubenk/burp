#include "burp.h"
#include "alloc.h"
#include "asfd.h"
#include "async.h"
#include "handy.h"
#include "iobuf.h"
#include "log.h"

void async_free(struct async **as)
{
	if(!as || !*as) return;
	free_v((void **)as);
}

static void async_settimers(struct async *as, int sec, int usec)
{
	as->setsec=sec;
	as->setusec=usec;
}

// The normal server and client processes will just exit on error within
// async_io, but the champ chooser server needs to manage client fds and
// remove them from its list if one of them had a problem.
static int asfd_problem(struct asfd *asfd)
{
	asfd->want_to_remove++;
	asfd->as->last_time=asfd->as->now;
	return -1;
}

static int async_io(struct async *as, int doread)
{
	int mfd=-1;
	fd_set fsr;
	fd_set fsw;
	fd_set fse;
	int dosomething=0;
	struct timeval tval;
	struct asfd *asfd;
	static int s=0;

	as->now=time(NULL);
	if(!as->last_time) as->last_time=as->now;

	if(as->doing_estimate) goto end;

	FD_ZERO(&fsr);
	FD_ZERO(&fsw);
	FD_ZERO(&fse);

	tval.tv_sec=as->setsec;
	tval.tv_usec=as->setusec;

	for(asfd=as->asfd; asfd; asfd=asfd->next)
	{
		if(asfd->fdtype==ASFD_FD_SERVER_PIPE_WRITE
		 || asfd->fdtype==ASFD_FD_CHILD_PIPE_WRITE
		 || asfd->fdtype==ASFD_FD_CLIENT_MONITOR_WRITE)
			asfd->doread=0;
		else
			asfd->doread=doread;
		asfd->dowrite=0;

		if(doread)
		{
			if(asfd->parse_readbuf(asfd))
				return asfd_problem(asfd);
			if(asfd->rbuf->buf || asfd->read_blocked_on_write)
				asfd->doread=0;
		}

		if(asfd->writebuflen && !asfd->write_blocked_on_read)
			asfd->dowrite++; // The write buffer is not yet empty.

		if(!asfd->doread && !asfd->dowrite) continue;

		add_fd_to_sets(asfd->fd, asfd->doread?&fsr:NULL,
			asfd->dowrite?&fsw:NULL, &fse, &mfd);

		dosomething++;
	}
	if(!dosomething) goto end;
/*
	for(asfd=as->asfd; asfd; asfd=asfd->next)
	{
		printf("%s: %d %d %d %d\n", asfd->desc,
			asfd->doread, asfd->dowrite,
			asfd->readbuflen, asfd->writebuflen);
	}
*/

	errno=0;
	s=select(mfd+1, &fsr, &fsw, &fse, &tval);
	if(errno==EAGAIN || errno==EINTR) goto end;

	if(s<0)
	{
		logp("select error in %s: %s\n", __func__,
			strerror(errno));
		as->last_time=as->now;
		return -1;
	}

	for(asfd=as->asfd; asfd; asfd=asfd->next)
	{
		if(FD_ISSET(asfd->fd, &fse))
		{
			switch(asfd->fdtype)
			{
				case ASFD_FD_SERVER_LISTEN_MAIN:
				case ASFD_FD_SERVER_LISTEN_STATUS:
					as->last_time=as->now;
					return -1;
				default:
					logp("%s: had an exception\n",
						asfd->desc);
					return asfd_problem(asfd);
			}
		}

		if(asfd->doread && FD_ISSET(asfd->fd, &fsr)) // Able to read.
		{
			asfd->network_timeout=asfd->max_network_timeout;
			switch(asfd->fdtype)
			{
				case ASFD_FD_SERVER_LISTEN_MAIN:
				case ASFD_FD_SERVER_LISTEN_STATUS:
					// Indicate to the caller that we have
					// a new incoming client.
					asfd->new_client++;
					break;
				default:
					if(asfd->do_read(asfd)
					  || asfd->parse_readbuf(asfd))
						return asfd_problem(asfd);
					break;
			}
		}

		if(asfd->dowrite && FD_ISSET(asfd->fd, &fsw)) // Able to write.
		{
			asfd->network_timeout=asfd->max_network_timeout;
			if(asfd->do_write(asfd))
				return asfd_problem(asfd);
		}
	
		if((!asfd->doread || !FD_ISSET(asfd->fd, &fsr))
		  && (!asfd->dowrite || !FD_ISSET(asfd->fd, &fsw)))
		{
			// Be careful to avoid 'read quick' mode.
			if((as->setsec || as->setusec)
			  && asfd->fdtype!=ASFD_FD_SERVER_LISTEN_MAIN
			  && asfd->fdtype!=ASFD_FD_SERVER_LISTEN_STATUS
			  && asfd->fdtype!=ASFD_FD_CHILD_PIPE_WRITE
			  && as->now-as->last_time>0
			  && as->now-as->last_time>0
			  && asfd->max_network_timeout>0
			  && asfd->network_timeout--<=0)
			{
				logp("%s: no activity for %d seconds.\n",
					asfd->desc, asfd->max_network_timeout);
				return asfd_problem(asfd);
			}
		}
	}

end:
	as->last_time=as->now;
	return 0;
}

static int async_read_write(struct async *as)
{
	return async_io(as, 1 /* Read too. */);
}

static int async_write(struct async *as)
{
	return async_io(as, 0 /* No read. */);
}

static int async_read_quick(struct async *as)
{
	int r;
	int savesec=as->setsec;
	int saveusec=as->setusec;
	as->setsec=0;
	as->setusec=0;
	r=as->read_write(as); // Maybe make an as->read(as) function some time.
	as->setsec=savesec;
	as->setusec=saveusec;
	return r;
}

static void async_asfd_add(struct async *as, struct asfd *asfd)
{
	struct asfd *x;
	if(!as->asfd)
	{
		as->asfd=asfd;
		return;
	}
	// Add to the end;
	for(x=as->asfd; x->next; x=x->next) { }
	x->next=asfd;
}

static void async_asfd_remove(struct async *as, struct asfd *asfd)
{
	struct asfd *l;
	if(!asfd) return;
	if(as->asfd==asfd)
	{
		as->asfd=as->asfd->next;
		return;
	}
	for(l=as->asfd; l; l=l->next)
	{
		if(l->next!=asfd) continue;
		l->next=asfd->next;
		return;
	}
}

void async_asfd_free_all(struct async **as)
{
	struct asfd *a=NULL;
	struct asfd *asfd=NULL;
	if(!as || !*as) return;
	for(asfd=(*as)->asfd; asfd; asfd=a)
	{
		a=asfd->next;
		asfd_free(&asfd);
	}
	async_free(as);
}

static int async_init(struct async *as, int estimate)
{
	as->setsec=1;
	as->setusec=0;
	as->last_time=0;
	as->doing_estimate=estimate;

	as->read_write=async_read_write;
	as->write=async_write;
	as->read_quick=async_read_quick;

	as->settimers=async_settimers;
	as->asfd_add=async_asfd_add;
	as->asfd_remove=async_asfd_remove;

	return 0;
}

struct async *async_alloc(void)
{
	struct async *as;
	if(!(as=(struct async *)calloc_w(1, sizeof(struct async), __func__)))
		return NULL;
	as->init=async_init;
	return as;
}
