// SPDX-License-Identifier: GPL-2.0+ OR Apache-2.0
/*
 * Copyright (C), 2022, Coolpad Group Limited.
 * Created by Yue Hu <huyue2@coolpad.com>
 */
#ifndef _LARGEFILE_SOURCE
#define _LARGEFILE_SOURCE
#endif
#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <unistd.h>
#include "erofs/err.h"
#include "erofs/inode.h"
#include "erofs/compress.h"
#include "erofs/print.h"
#include "erofs/internal.h"
#include "erofs/fragments.h"

struct erofs_fragment_dedupe_item {
	struct list_head	list;
	unsigned int		length, nr_dup;
	erofs_off_t		pos;
	u8			data[];
};

#define EROFS_TOF_HASHLEN		16

#define FRAGMENT_HASHSIZE		65536
#define FRAGMENT_HASH(c)		((c) & (FRAGMENT_HASHSIZE - 1))

static struct list_head dupli_frags[FRAGMENT_HASHSIZE];

static FILE *packedfile;
const char *frags_packedname = "packed_file";

#ifndef HAVE_LSEEK64
#define erofs_lseek64 lseek
#else
#define erofs_lseek64 lseek64
#endif

static int z_erofs_fragments_dedupe_find(struct erofs_inode *inode, int fd,
					 u32 crc)
{
	struct erofs_fragment_dedupe_item *cur, *di = NULL;
	struct list_head *head;
	u8 *data;
	unsigned int length, e2;
	int ret;

	head = &dupli_frags[FRAGMENT_HASH(crc)];
	if (list_empty(head))
		return 0;

	/* XXX: no need to read so much for smaller? */
	if (inode->i_size < EROFS_CONFIG_COMPR_MAX_SZ)
		length = inode->i_size;
	else
		length = EROFS_CONFIG_COMPR_MAX_SZ;

	data = malloc(length);
	if (!data)
		return -ENOMEM;

	if (erofs_lseek64(fd, inode->i_size - length, SEEK_SET) < 0) {
		ret = -errno;
		goto out;
	}

	ret = read(fd, data, length);
	if (ret != length) {
		ret = -errno;
		goto out;
	}

	DBG_BUGON(length <= EROFS_TOF_HASHLEN);
	e2 = length - EROFS_TOF_HASHLEN;

	list_for_each_entry(cur, head, list) {
		unsigned int e1, mn, i = 0;

		DBG_BUGON(cur->length <= EROFS_TOF_HASHLEN);
		e1 = cur->length - EROFS_TOF_HASHLEN;

		if (memcmp(cur->data + e1, data + e2, EROFS_TOF_HASHLEN))
			continue;

		mn = min(e1, e2);
		while (i < mn && cur->data[e1 - i - 1] == data[e2 - i - 1])
			++i;

		if (i && (!di || i + EROFS_TOF_HASHLEN > di->nr_dup)) {
			cur->nr_dup = i + EROFS_TOF_HASHLEN;
			di = cur;

			/* full match */
			if (i == mn)
				break;
		}
	}
	if (!di)
		goto out;

	DBG_BUGON(di->length < di->nr_dup);

	inode->fragment_size = di->nr_dup;
	inode->fragmentoff = di->pos + di->length - di->nr_dup;

	erofs_dbg("Dedupe %u tail data at %llu", inode->fragment_size,
		  inode->fragmentoff | 0ULL);
out:
	free(data);
	return ret;
}

int z_erofs_fragments_dedupe(struct erofs_inode *inode, int fd, u32 *tofcrc)
{
	u8 data_to_hash[EROFS_TOF_HASHLEN];
	int ret;

	if (inode->i_size <= EROFS_TOF_HASHLEN)
		return 0;

	if (erofs_lseek64(fd, inode->i_size - EROFS_TOF_HASHLEN, SEEK_SET) < 0)
		return -errno;

	ret = read(fd, data_to_hash, EROFS_TOF_HASHLEN);
	if (ret != EROFS_TOF_HASHLEN)
		return -errno;

	*tofcrc = erofs_crc32c(~0, data_to_hash, EROFS_TOF_HASHLEN);
	ret = z_erofs_fragments_dedupe_find(inode, fd, *tofcrc);
	if (ret < 0)
		return ret;
	ret = lseek(fd, 0, SEEK_SET);
	if (ret < 0)
		return -errno;
	return 0;
}

static int z_erofs_fragments_dedupe_insert(void *data, unsigned int len,
					   erofs_off_t pos, u32 crc)
{
	struct erofs_fragment_dedupe_item *di;

	if (len <= EROFS_TOF_HASHLEN)
		return 0;

	di = malloc(sizeof(*di) + len);
	if (!di)
		return -ENOMEM;

	memcpy(di->data, data, len);
	di->length = len;
	di->pos = pos;
	di->nr_dup = 0;

	list_add_tail(&di->list, &dupli_frags[FRAGMENT_HASH(crc)]);
	return 0;
}

static void z_erofs_fragments_dedupe_init(void)
{
	unsigned int i;

	for (i = 0; i < FRAGMENT_HASHSIZE; ++i)
		init_list_head(&dupli_frags[i]);
}

static void z_erofs_fragments_dedupe_exit(void)
{
	struct erofs_fragment_dedupe_item *di, *n;
	struct list_head *head;
	unsigned int i;

	for (i = 0; i < FRAGMENT_HASHSIZE; ++i) {
		head = &dupli_frags[i];

		list_for_each_entry_safe(di, n, head, list)
			free(di);
	}
}

void z_erofs_fragments_commit(struct erofs_inode *inode)
{
	if (!inode->fragment_size)
		return;
	/*
	 * If the packed inode is larger than 4GiB, the full fragmentoff
	 * will be recorded by switching to the noncompact layout anyway.
	 */
	if (inode->fragmentoff >> 32)
		inode->datalayout = EROFS_INODE_FLAT_COMPRESSION_LEGACY;

	inode->z_advise |= Z_EROFS_ADVISE_FRAGMENT_PCLUSTER;
	erofs_sb_set_fragments();
}

int z_erofs_pack_fragments(struct erofs_inode *inode, void *data,
			   unsigned int len, u32 tofcrc)
{
#ifdef HAVE_FTELLO64
	off64_t offset = ftello64(packedfile);
#else
	off_t offset = ftello(packedfile);
#endif
	int ret;

	if (offset < 0)
		return -errno;

	inode->fragmentoff = (erofs_off_t)offset;
	inode->fragment_size = len;

	if (fwrite(data, len, 1, packedfile) != 1)
		return -EIO;

	erofs_dbg("Recording %u fragment data at %lu", inode->fragment_size,
		  inode->fragmentoff);

	ret = z_erofs_fragments_dedupe_insert(data, len, inode->fragmentoff,
					      tofcrc);
	if (ret)
		return ret;
	return len;
}

struct erofs_inode *erofs_mkfs_build_fragments(void)
{
	fflush(packedfile);

	return erofs_mkfs_build_special_from_fd(fileno(packedfile),
						frags_packedname);
}

void erofs_fragments_exit(void)
{
	if (packedfile)
		fclose(packedfile);

	z_erofs_fragments_dedupe_exit();
}

int erofs_fragments_init(void)
{
#ifdef HAVE_TMPFILE64
	packedfile = tmpfile64();
#else
	packedfile = tmpfile();
#endif
	if (!packedfile)
		return -ENOMEM;

	z_erofs_fragments_dedupe_init();
	return 0;
}
