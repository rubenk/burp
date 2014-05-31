#include "include.h"
#include "../../server/burp2/restore.h"
#include "../../server/monitor/status_client.h"

#include <librsync.h>

static int inflate_or_link_oldfile(struct asfd *asfd, const char *oldpath,
	const char *infpath, struct conf *cconf, int compression)
{
	int ret=0;
	struct stat statp;

	if(lstat(oldpath, &statp))
	{
		logp("could not lstat %s\n", oldpath);
		return -1;
	}

	if(dpthl_is_compressed(compression, oldpath))
	{
		//logp("inflating...\n");

		if(!statp.st_size)
		{
			FILE *dest;
			// Empty file - cannot inflate.
			// Just open and close the destination and we have
			// duplicated a zero length file.
			logp("asked to inflate zero length file: %s\n", oldpath);
			if(!(dest=open_file(infpath, "wb")))
			{
				close_fp(&dest);
				return -1;
			}
			close_fp(&dest);
			return 0;
		}

		if((ret=zlib_inflate(asfd, oldpath, infpath, cconf)))
			logp("zlib_inflate returned: %d\n", ret);
	}
	else
	{
		// Not compressed - just hard link it.
		if(do_link(oldpath, infpath, &statp, cconf,
			1 /* allow overwrite of infpath */))
				return -1;
	}
	return ret;
}

static int send_file(struct asfd *asfd, struct sbuf *sb,
	int patches, const char *best,
	unsigned long long *bytes, struct conf *cconf)
{
	int ret=0;
	size_t datalen=0;
	FILE *fp=NULL;
	if(open_file_for_sendl(asfd, NULL, &fp, best, sb->winattr, &datalen,
		1 /* no O_NOATIME */, cconf)) return -1;
	//logp("sending: %s\n", best);
	if(asfd->write(asfd, &sb->path))
		ret=-1;
	else if(patches)
	{
		// If we did some patches, the resulting file
		// is not gzipped. Gzip it during the send. 
		ret=send_whole_file_gzl(asfd, best, sb->burp1->datapth.buf,
			1, bytes, NULL,
			cconf, 9, NULL, fp, NULL, 0, -1);
	}
	else
	{
		// If it was encrypted, it may or may not have been compressed
		// before encryption. Send it as it as, and let the client
		// sort it out.
		if(sb->path.cmd==CMD_ENC_FILE
		  || sb->path.cmd==CMD_ENC_METADATA
		  || sb->path.cmd==CMD_ENC_VSS
		  || sb->path.cmd==CMD_ENC_VSS_T
		  || sb->path.cmd==CMD_EFS_FILE)
		{
			ret=send_whole_filel(asfd, sb->path.cmd, best,
				sb->burp1->datapth.buf, 1, bytes,
				cconf, NULL, fp, NULL, 0, -1);
		}
		// It might have been stored uncompressed. Gzip it during
		// the send. If the client knew what kind of file it would be
		// receiving, this step could disappear.
		else if(!dpthl_is_compressed(sb->compression,
			sb->burp1->datapth.buf))
		{
			ret=send_whole_file_gzl(asfd,
				best, sb->burp1->datapth.buf, 1, bytes,
				NULL, cconf, 9, NULL, fp, NULL, 0, -1);
		}
		else
		{
			// If we did not do some patches, the resulting
			// file might already be gzipped. Send it as it is.
			ret=send_whole_filel(asfd, sb->path.cmd, best,
				sb->burp1->datapth.buf, 1, bytes,
				cconf, NULL, fp, NULL, 0, -1);
		}
	}
	close_file_for_sendl(NULL, &fp, asfd);
	return ret;
}

static int verify_file(struct asfd *asfd, struct sbuf *sb,
	int patches, const char *best,
	unsigned long long *bytes, struct conf *cconf)
{
	MD5_CTX md5;
	size_t b=0;
	const char *cp=NULL;
	const char *newsum=NULL;
	unsigned char in[ZCHUNK];
	unsigned char checksum[MD5_DIGEST_LENGTH+1];
	unsigned long long cbytes=0;
	if(!(cp=strrchr(sb->burp1->endfile.buf, ':')))
	{
		logw(asfd, cconf, "%s has no md5sum!\n", sb->burp1->datapth.buf);
		return 0;
	}
	cp++;
	if(!MD5_Init(&md5))
	{
		logp("MD5_Init() failed\n");
		return -1;
	}
	if(patches
	  || sb->path.cmd==CMD_ENC_FILE
	  || sb->path.cmd==CMD_ENC_METADATA
	  || sb->path.cmd==CMD_EFS_FILE
	  || sb->path.cmd==CMD_ENC_VSS
	  || (!patches && !dpthl_is_compressed(sb->compression, best)))
	{
		// If we did some patches or encryption, or the compression
		// was turned off, the resulting file is not gzipped.
		FILE *fp=NULL;
		if(!(fp=open_file(best, "rb")))
		{
			logw(asfd, cconf, "could not open %s\n", best);
			return 0;
		}
		while((b=fread(in, 1, ZCHUNK, fp))>0)
		{
			cbytes+=b;
			if(!MD5_Update(&md5, in, b))
			{
				logp("MD5_Update() failed\n");
				close_fp(&fp);
				return -1;
			}
		}
		if(!feof(fp))
		{
			logw(asfd, cconf, "error while reading %s\n", best);
			close_fp(&fp);
			return 0;
		}
		close_fp(&fp);
	}
	else
	{
		gzFile zp=NULL;
		if(!(zp=gzopen_file(best, "rb")))
		{
			logw(asfd, cconf, "could not gzopen %s\n", best);
			return 0;
		}
		while((b=gzread(zp, in, ZCHUNK))>0)
		{
			cbytes+=b;
			if(!MD5_Update(&md5, in, b))
			{
				logp("MD5_Update() failed\n");
				gzclose_fp(&zp);
				return -1;
			}
		}
		if(!gzeof(zp))
		{
			logw(asfd, cconf, "error while gzreading %s\n", best);
			gzclose_fp(&zp);
			return 0;
		}
		gzclose_fp(&zp);
	}
	if(!MD5_Final(checksum, &md5))
	{
		logp("MD5_Final() failed\n");
		return -1;
	}
	newsum=get_checksum_str(checksum);

	if(strcmp(newsum, cp))
	{
		logp("%s %s\n", newsum, cp);
		logw(asfd, cconf, "md5sum for '%s (%s)' did not match!\n",
			sb->path.buf, sb->burp1->datapth.buf);
		logp("md5sum for '%s (%s)' did not match!\n",
			sb->path.buf, sb->burp1->datapth.buf);
		return 0;
	}
	*bytes+=cbytes;

	// Just send the file name to the client, so that it can show cntr.
	if(asfd->write(asfd, &sb->path)) return -1;
	return 0;
}

// a = length of struct bu array
// i = position to restore from
static int restore_file(struct asfd *asfd, struct bu *arr, int a, int i,
	struct sbuf *sb, int act, struct sdirs *sdirs, struct conf *cconf)
{
	int x=0;
	static char *tmppath1=NULL;
	static char *tmppath2=NULL;

	if((!tmppath1 && !(tmppath1=prepend_s(sdirs->client, "tmp1")))
	  || (!tmppath2 && !(tmppath2=prepend_s(sdirs->client, "tmp2"))))
		return -1;

	// Go up the array until we find the file in the data directory.
	for(x=i; x<a; x++)
	{
		char *path=NULL;
		struct stat statp;
		if(!(path=prepend_s(arr[x].data, sb->burp1->datapth.buf)))
		{
			log_and_send_oom(asfd, __func__);
			return -1;
		}

		//printf("server file: %s\n", path);

		if(lstat(path, &statp) || !S_ISREG(statp.st_mode))
		{
			free(path);
			continue;
		}
		else
		{
			int patches=0;
			struct stat dstatp;
			const char *tmp=NULL;
			const char *best=NULL;
			unsigned long long bytes=0;

			best=path;
			tmp=tmppath1;
			// Now go down the array, applying any deltas.
			for(x-=1; x>=i; x--)
			{
				char *dpath=NULL;

				if(!(dpath=prepend_s(arr[x].delta,
					sb->burp1->datapth.buf)))
				{
					log_and_send_oom(asfd, __func__);
					free(path);
					return -1;
				}

				if(lstat(dpath, &dstatp)
				  || !S_ISREG(dstatp.st_mode))
				{
					free(dpath);
					continue;
				}

				if(!patches)
				{
					// Need to gunzip the first one.
					if(inflate_or_link_oldfile(asfd,
						best, tmp,
						cconf, sb->compression))
					{
						logp("error when inflating %s\n", best);
						free(path);
						free(dpath);
						return -1;
					}
					best=tmp;
					if(tmp==tmppath1) tmp=tmppath2;
					else tmp=tmppath1;
				}

				if(do_patch(asfd, best, dpath, tmp,
				  0 /* do not gzip the result */,
				  sb->compression /* from the manifest */,
				  cconf))
				{
					char msg[256]="";
					snprintf(msg, sizeof(msg),
						"error when patching %s\n",
							path);
					log_and_send(asfd, msg);
					free(path);
					free(dpath);
					return -1;
				}

				best=tmp;
				if(tmp==tmppath1) tmp=tmppath2;
				else tmp=tmppath1;
				unlink(tmp);
				patches++;
			}

			if(act==ACTION_RESTORE)
			{
				if(send_file(asfd, sb,
					patches, best, &bytes, cconf))
				{
					free(path);
					return -1;
				}
				else
				{
					cntr_add(cconf->cntr,
						sb->path.cmd, 0);
					cntr_add_bytes(cconf->cntr,
                 			  strtoull(sb->burp1->endfile.buf,
						NULL, 10));
				}
			}
			else if(act==ACTION_VERIFY)
			{
				if(verify_file(asfd, sb, patches,
					best, &bytes, cconf))
				{
					free(path);
					return -1;
				}
				else
				{
					cntr_add(cconf->cntr,
						sb->path.cmd, 0);
					cntr_add_bytes(cconf->cntr,
                 			  strtoull(sb->burp1->endfile.buf,
						NULL, 10));
				}
			}
			cntr_add_sentbytes(cconf->cntr, bytes);
			free(path);
			return 0;
		}
	}

	logw(asfd, cconf, "restore could not find %s (%s)\n",
		sb->path.buf, sb->burp1->datapth.buf);
	//return -1;
	return 0;
}

static int restore_sbufl(struct asfd *asfd, struct sbuf *sb, struct bu *arr,
	int a, int i, enum action act, struct sdirs *sdirs,
	 char status, struct conf *cconf)
{
	//printf("%s: %s\n", act==ACTION_RESTORE?"restore":"verify", sb->path.buf);
	if(write_status(status, sb->path.buf, cconf)) return -1;

	if((sb->burp1->datapth.buf && asfd->write(asfd, &(sb->burp1->datapth)))
	  || asfd->write(asfd, &sb->attr))
		return -1;
	else if(sb->path.cmd==CMD_FILE
	  || sb->path.cmd==CMD_ENC_FILE
	  || sb->path.cmd==CMD_METADATA
	  || sb->path.cmd==CMD_ENC_METADATA
	  || sb->path.cmd==CMD_VSS
	  || sb->path.cmd==CMD_ENC_VSS
	  || sb->path.cmd==CMD_VSS_T
	  || sb->path.cmd==CMD_ENC_VSS_T
	  || sb->path.cmd==CMD_EFS_FILE)
	{
		return restore_file(asfd, arr, a, i, sb, act, sdirs, cconf);
	}
	else
	{
		if(asfd->write(asfd, &sb->path))
			return -1;
		// If it is a link, send what
		// it points to.
		else if(sbuf_is_link(sb)
		  && asfd->write(asfd, &sb->link)) return -1;
		cntr_add(cconf->cntr, sb->path.cmd, 0);
	}
	return 0;
}

static int do_restore_end(struct asfd *asfd, enum action act, struct conf *conf)
{
	int ret=-1;
	struct iobuf *rbuf=asfd->rbuf;

	if(asfd->write_str(asfd, CMD_GEN, "restoreend"))
		goto end;

	while(1)
	{
		iobuf_free_content(rbuf);
		if(asfd->read(asfd)) goto end;
		else if(rbuf->cmd==CMD_GEN
		  && !strcmp(rbuf->buf, "restoreend ok"))
		{
			logp("got restoreend ok\n");
			break;
		}
		else if(rbuf->cmd==CMD_WARNING)
		{
			logp("WARNING: %s\n", rbuf->buf);
			cntr_add(conf->cntr, rbuf->cmd, 0);
		}
		else if(rbuf->cmd==CMD_INTERRUPT)
		{
			// ignore - client wanted to interrupt a file
		}
		else
		{
			iobuf_log_unexpected(rbuf, __func__);
			goto end;
		}
	}
	ret=0;
end:
	iobuf_free_content(rbuf);
	return ret;
}

static int restore_ent(struct asfd *asfd, struct sbuf **sb,
	struct sbuf ***sblist, int *scount, struct bu *arr, int a, int i,
	enum action act, struct sdirs *sdirs, char status, struct conf *cconf)
{
	int s=0;
	int ret=-1;

	// Check if we have any directories waiting to be restored.
	for(s=(*scount)-1; s>=0; s--)
	{
		if(is_subdir((*sblist)[s]->path.buf, (*sb)->path.buf))
		{
			// We are still in a subdir.
			//printf(" subdir (%s %s)\n",
			// (*sblist)[s]->path, (*sb)->path);
			break;
		}
		else
		{
			// Can now restore sblist[s] because nothing else is
			// fiddling in a subdirectory.
			if(restore_sbufl(asfd, (*sblist)[s], arr, a, i,
				act, sdirs, status, cconf))
					goto end;
			else if(del_from_sbufl_arr(sblist, scount))
				goto end;
		}
	}

	/* If it is a directory, need to remember it and restore it later, so
	   that the permissions come out right. */
	/* Meta data of directories will also have the stat stuff set to be a
	   directory, so will also come out at the end. */
	if(S_ISDIR((*sb)->statp.st_mode))
	{
		if(add_to_sbufl_arr(sblist, *sb, scount))
			goto end;
		// Allocate a new sb to carry on with.
		if(!(*sb=sbuf_alloc(cconf)))
			goto end;
	}
	else if(restore_sbufl(asfd, *sb, arr, a, i, act, sdirs, status, cconf))
		goto end;
	ret=0;
end:
	return ret;
}

static int setup_cntr(struct asfd *asfd, const char *manifest,
	regex_t *regex, int srestore,
	enum action act, char status, struct conf *cconf)
{
	int ars=0;
	int ret=-1;
	gzFile zp;
	struct sbuf *sb=NULL;
	if(!(sb=sbuf_alloc(cconf))) goto end;
	if(!(zp=gzopen_file(manifest, "rb")))
	{
		log_and_send(asfd, "could not open manifest");
		goto end;
	}
	while(1)
	{
		if((ars=sbufl_fill(sb, asfd, NULL, zp, cconf->cntr)))
		{
			if(ars<0) goto end;
			// ars==1 means end ok
			break;
		}
		else
		{
			if((!srestore || check_srestore(cconf, sb->path.buf))
			  && check_regex(regex, sb->path.buf))
			{
				cntr_add_phase1(cconf->cntr,
					sb->path.cmd, 0);
				if(sb->burp1->endfile.buf)
					cntr_add_val(cconf->cntr,
						CMD_BYTES_ESTIMATED,
				    		strtoull(sb->burp1->endfile.buf,
							NULL, 10), 0);
			}
		}
		sbuf_free_content(sb);
	}
	ret=0;
end:
	sbuf_free(&sb);
	gzclose_fp(&zp);
	return ret;
}

static int actual_restore(struct asfd *asfd,
	struct bu *arr, int a, int i,
	const char *manifest, regex_t *regex, int srestore,
	enum action act, struct sdirs *sdirs, char status,
	struct conf *cconf)
{
	int s=0;
	int ret=-1;
	struct sbuf *sb=NULL;
	// For out-of-sequence directory restoring so that the
	// timestamps come out right:
	int scount=0;
	struct sbuf **sblist=NULL;
	struct iobuf *rbuf=asfd->rbuf;
	gzFile zp;

	if(!(sb=sbuf_alloc(cconf))) goto end;
	if(!(zp=gzopen_file(manifest, "rb")))
	{
		log_and_send(asfd, "could not open manifest");
		goto end;
	}

	while(1)
	{
		int ars=0;
		iobuf_free_content(rbuf);
		if(asfd->as->read_quick(asfd->as))
		{
			logp("read quick error\n");
			goto end;
		}
		if(rbuf->buf)
		{
			//logp("got read quick\n");
			if(rbuf->cmd==CMD_WARNING)
			{
				logp("WARNING: %s\n", rbuf->buf);
				cntr_add(cconf->cntr, rbuf->cmd, 0);
				continue;
			}
			else if(rbuf->cmd==CMD_INTERRUPT)
			{
				// Client wanted to interrupt the
				// sending of a file. But if we are
				// here, we have already moved on.
				// Ignore.
				continue;
			}
			else
			{
				iobuf_log_unexpected(rbuf, __func__);
				goto end;
			}
		}

		if((ars=sbufl_fill(sb, asfd, NULL, zp, cconf->cntr)))
		{
			if(ars<0) goto end;
			break;
		}
		else
		{
			if((!srestore
			    || check_srestore(cconf, sb->path.buf))
			  && check_regex(regex, sb->path.buf)
			  && restore_ent(asfd, &sb, &sblist, &scount,
				arr, a, i, act, sdirs, status, cconf))
					goto end;
		}
		sbuf_free_content(sb);
	}
	// Restore any directories that are left in the list.
	if(!ret) for(s=scount-1; s>=0; s--)
	{
		if(restore_sbufl(asfd, sblist[s], arr, a, i,
			act, sdirs, status, cconf))
				goto end;
	}

	ret=do_restore_end(asfd, act, cconf);

	cntr_print(cconf->cntr, act);

	cntr_stats_to_file(cconf->cntr, arr[i].path, act);

end:
	iobuf_free_content(rbuf);
	gzclose_fp(&zp);
	sbuf_free(&sb);
	free_sbufls(sblist, scount);
	return ret;
}

// a = length of struct bu array
// i = position to restore from
int restore_manifest_burp1(struct asfd *asfd,
	struct bu *arr, int a, int i,
	regex_t *regex, int srestore, enum action act, struct sdirs *sdirs,
	char **dir_for_notify, struct conf *cconf)
{
	int ret=-1;
	char *manifest=NULL;
	char *datadir=NULL;
	char *logpath=NULL;
	char *logpathz=NULL;
	// For sending status information up to the server.
	char status=STATUS_RESTORING;

	if(act==ACTION_RESTORE) status=STATUS_RESTORING;
	else if(act==ACTION_VERIFY) status=STATUS_VERIFYING;

	if((act==ACTION_RESTORE
		&& !(logpath=prepend_s(arr[i].path, "restorelog")))
	 || (act==ACTION_RESTORE
		&& !(logpathz=prepend_s(arr[i].path, "restorelog.gz")))
	 || (act==ACTION_VERIFY
		&& !(logpath=prepend_s(arr[i].path, "verifylog")))
	 || (act==ACTION_VERIFY
		&& !(logpathz=prepend_s(arr[i].path, "verifylog.gz")))
	 || !(manifest=prepend_s(arr[i].path, "manifest.gz")))
	{
		log_and_send_oom(asfd, __func__);
		goto end;
	}
	else if(set_logfp(logpath, cconf))
	{
		char msg[256]="";
		snprintf(msg, sizeof(msg),
			"could not open log file: %s", logpath);
		log_and_send(asfd, msg);
		goto end;
	}

	*dir_for_notify=strdup(arr[i].path);

	log_restore_settings(cconf, srestore);

	// First, do a pass through the manifest to set up cntr.
	// This is the equivalent of a phase1 scan during backup.

	if(setup_cntr(asfd, manifest, regex, srestore,
		act, status, cconf))
			goto end;

	if(cconf->send_client_cntr && cntr_send(cconf->cntr))
		goto end;

	// Now, do the actual restore.
	if(actual_restore(asfd, arr, a, i, manifest,
		regex, srestore, act, sdirs, status, cconf))
			goto end;

	ret=0;
end:
	set_logfp(NULL, cconf);
	compress_file(logpath, logpathz, cconf);
	if(manifest) free(manifest);
	if(datadir) free(datadir);
	if(logpath) free(logpath);
	if(logpathz) free(logpathz);
	return ret;
}