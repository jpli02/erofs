// SPDX-License-Identifier: GPL-2.0+ OR Apache-2.0
/*
 * erofs-utils/lib/blobchunk.c
 *
 * Copyright (C) 2021, Alibaba Cloud
 */
#define _GNU_SOURCE
#include "erofs/hashmap.h"
#include "erofs/blobchunk.h"
#include "erofs/block_list.h"
#include "erofs/cache.h"
#include "erofs/io.h"
#include "sha256.h"
#include <unistd.h>

struct erofs_blobchunk {
	union {
		struct hashmap_entry ent;
		struct list_head list;
	};
	char		sha256[32];
	unsigned int	device_id;
	union {
		erofs_off_t	chunksize;
		erofs_off_t	sourceoffset;
	};
	erofs_blk_t	blkaddr;
};

static struct hashmap blob_hashmap;
static FILE *blobfile;
static erofs_blk_t remapped_base;
static erofs_off_t datablob_size;
static bool multidev;
static struct erofs_buffer_head *bh_devt;
struct erofs_blobchunk erofs_holechunk = {
	.blkaddr = EROFS_NULL_ADDR,
};
static LIST_HEAD(unhashed_blobchunks);

static struct erofs_blobchunk *erofs_get_unhashed_chunk(unsigned int device_id,
		erofs_blk_t blkaddr, erofs_off_t sourceoffset)
{
	struct erofs_blobchunk *chunk;

	chunk = calloc(1, sizeof(struct erofs_blobchunk));
	if (!chunk)
		return ERR_PTR(-ENOMEM);

	chunk->device_id = device_id;
	chunk->blkaddr = blkaddr;
	chunk->sourceoffset = sourceoffset;
	list_add_tail(&chunk->list, &unhashed_blobchunks);
	return chunk;
}

static struct erofs_blobchunk *erofs_blob_getchunk(struct erofs_sb_info *sbi,
						u8 *buf, erofs_off_t chunksize)
{
	static u8 zeroed[EROFS_MAX_BLOCK_SIZE];
	struct erofs_blobchunk *chunk;
	unsigned int hash, padding;
	u8 sha256[32];
	erofs_off_t blkpos;
	int ret;

	erofs_sha256(buf, chunksize, sha256);
	hash = memhash(sha256, sizeof(sha256));
	chunk = hashmap_get_from_hash(&blob_hashmap, hash, sha256);
	if (chunk) {
		DBG_BUGON(chunksize != chunk->chunksize);
		erofs_dbg("Found duplicated chunk at %u", chunk->blkaddr);
		return chunk;
	}

	chunk = malloc(sizeof(struct erofs_blobchunk));
	if (!chunk)
		return ERR_PTR(-ENOMEM);

	chunk->chunksize = chunksize;
	memcpy(chunk->sha256, sha256, sizeof(sha256));
	blkpos = ftell(blobfile);
	DBG_BUGON(erofs_blkoff(sbi, blkpos));

	if (sbi->extra_devices)
		chunk->device_id = 1;
	else
		chunk->device_id = 0;
	chunk->blkaddr = erofs_blknr(sbi, blkpos);

	erofs_dbg("Writing chunk (%u bytes) to %u", chunksize, chunk->blkaddr);
	ret = fwrite(buf, chunksize, 1, blobfile);
	padding = erofs_blkoff(sbi, chunksize);
	if (ret == 1) {
		padding = erofs_blkoff(sbi, chunksize);
		if (padding) {
			padding = erofs_blksiz(sbi) - padding;
			ret = fwrite(zeroed, padding, 1, blobfile);
		}
	}

	if (ret < 1) {
		free(chunk);
		return ERR_PTR(-ENOSPC);
	}

	hashmap_entry_init(&chunk->ent, hash);
	hashmap_add(&blob_hashmap, chunk);
	return chunk;
}

static int erofs_blob_hashmap_cmp(const void *a, const void *b,
				  const void *key)
{
	const struct erofs_blobchunk *ec1 =
			container_of((struct hashmap_entry *)a,
				     struct erofs_blobchunk, ent);
	const struct erofs_blobchunk *ec2 =
			container_of((struct hashmap_entry *)b,
				     struct erofs_blobchunk, ent);

	return memcmp(ec1->sha256, key ? key : ec2->sha256,
		      sizeof(ec1->sha256));
}

int erofs_blob_write_chunk_indexes(struct erofs_inode *inode,
				   erofs_off_t off)
{
	struct erofs_inode_chunk_index idx = {0};
	erofs_blk_t extent_start = EROFS_NULL_ADDR;
	erofs_blk_t extent_end, chunkblks;
	erofs_off_t source_offset;
	unsigned int dst, src, unit;
	bool first_extent = true;

	if (inode->u.chunkformat & EROFS_CHUNK_FORMAT_INDEXES)
		unit = sizeof(struct erofs_inode_chunk_index);
	else
		unit = EROFS_BLOCK_MAP_ENTRY_SIZE;

	chunkblks = 1U << (inode->u.chunkformat & EROFS_CHUNK_FORMAT_BLKBITS_MASK);
	for (dst = src = 0; dst < inode->extent_isize;
	     src += sizeof(void *), dst += unit) {
		struct erofs_blobchunk *chunk;

		chunk = *(void **)(inode->chunkindexes + src);

		if (chunk->blkaddr == EROFS_NULL_ADDR) {
			idx.blkaddr = EROFS_NULL_ADDR;
		} else if (chunk->device_id) {
			DBG_BUGON(!(inode->u.chunkformat & EROFS_CHUNK_FORMAT_INDEXES));
			idx.blkaddr = chunk->blkaddr;
			extent_start = EROFS_NULL_ADDR;
		} else {
			idx.blkaddr = remapped_base + chunk->blkaddr;
		}

		if (extent_start == EROFS_NULL_ADDR ||
		    idx.blkaddr != extent_end) {
			if (extent_start != EROFS_NULL_ADDR) {
				tarerofs_blocklist_write(extent_start,
						extent_end - extent_start,
						source_offset);
				erofs_droid_blocklist_write_extent(inode,
					extent_start,
					extent_end - extent_start,
					first_extent, false);
				first_extent = false;
			}
			extent_start = idx.blkaddr;
			source_offset = chunk->sourceoffset;
		}
		extent_end = idx.blkaddr + chunkblks;
		idx.device_id = cpu_to_le16(chunk->device_id);
		idx.blkaddr = cpu_to_le32(idx.blkaddr);

		if (unit == EROFS_BLOCK_MAP_ENTRY_SIZE)
			memcpy(inode->chunkindexes + dst, &idx.blkaddr, unit);
		else
			memcpy(inode->chunkindexes + dst, &idx, sizeof(idx));
	}
	off = roundup(off, unit);
	if (extent_start != EROFS_NULL_ADDR)
		tarerofs_blocklist_write(extent_start, extent_end - extent_start,
					 source_offset);
	erofs_droid_blocklist_write_extent(inode, extent_start,
			extent_start == EROFS_NULL_ADDR ?
					0 : extent_end - extent_start,
					   first_extent, true);

	return dev_write(inode->sbi, inode->chunkindexes, off, inode->extent_isize);
}

int erofs_blob_mergechunks(struct erofs_inode *inode, unsigned int chunkbits,
			   unsigned int new_chunkbits)
{
	struct erofs_sb_info *sbi = inode->sbi;
	unsigned int dst, src, unit, count;

	if (new_chunkbits - sbi->blkszbits > EROFS_CHUNK_FORMAT_BLKBITS_MASK)
		new_chunkbits = EROFS_CHUNK_FORMAT_BLKBITS_MASK + sbi->blkszbits;
	if (chunkbits >= new_chunkbits)		/* no need to merge */
		goto out;

	if (inode->u.chunkformat & EROFS_CHUNK_FORMAT_INDEXES)
		unit = sizeof(struct erofs_inode_chunk_index);
	else
		unit = EROFS_BLOCK_MAP_ENTRY_SIZE;

	count = round_up(inode->i_size, 1ULL << new_chunkbits) >> new_chunkbits;
	for (dst = src = 0; dst < count; ++dst) {
		*((void **)inode->chunkindexes + dst) =
			*((void **)inode->chunkindexes + src);
		src += 1U << (new_chunkbits - chunkbits);
	}

	DBG_BUGON(count * unit >= inode->extent_isize);
	inode->extent_isize = count * unit;
	chunkbits = new_chunkbits;
out:
	inode->u.chunkformat = (chunkbits - sbi->blkszbits) |
		(inode->u.chunkformat & ~EROFS_CHUNK_FORMAT_BLKBITS_MASK);
	return 0;
}

int erofs_blob_write_chunked_file(struct erofs_inode *inode, int fd)
{
	struct erofs_sb_info *sbi = inode->sbi;
	unsigned int chunkbits = cfg.c_chunkbits;
	unsigned int count, unit;
	struct erofs_blobchunk *chunk, *lastch;
	struct erofs_inode_chunk_index *idx;
	erofs_off_t pos, len, chunksize;
	erofs_blk_t lb, minextblks;
	u8 *chunkdata;
	int ret;

#ifdef SEEK_DATA
	/* if the file is fully sparsed, use one big chunk instead */
	if (lseek(fd, 0, SEEK_DATA) < 0 && errno == ENXIO) {
		chunkbits = ilog2(inode->i_size - 1) + 1;
		if (chunkbits < sbi->blkszbits)
			chunkbits = sbi->blkszbits;
	}
#endif
	if (chunkbits - sbi->blkszbits > EROFS_CHUNK_FORMAT_BLKBITS_MASK)
		chunkbits = EROFS_CHUNK_FORMAT_BLKBITS_MASK + sbi->blkszbits;
	chunksize = 1ULL << chunkbits;
	count = DIV_ROUND_UP(inode->i_size, chunksize);

	if (sbi->extra_devices)
		inode->u.chunkformat |= EROFS_CHUNK_FORMAT_INDEXES;
	if (inode->u.chunkformat & EROFS_CHUNK_FORMAT_INDEXES)
		unit = sizeof(struct erofs_inode_chunk_index);
	else
		unit = EROFS_BLOCK_MAP_ENTRY_SIZE;

	chunkdata = malloc(chunksize);
	if (!chunkdata)
		return -ENOMEM;

	inode->extent_isize = count * unit;
	inode->chunkindexes = malloc(count * max(sizeof(*idx), sizeof(void *)));
	if (!inode->chunkindexes) {
		ret = -ENOMEM;
		goto err;
	}
	idx = inode->chunkindexes;

	lastch = NULL;
	minextblks = BLK_ROUND_UP(sbi, inode->i_size);
	for (pos = 0; pos < inode->i_size; pos += len) {
#ifdef SEEK_DATA
		off_t offset = lseek(fd, pos, SEEK_DATA);

		if (offset < 0) {
			if (errno != ENXIO)
				offset = pos;
			else
				offset = ((pos >> chunkbits) + 1) << chunkbits;
		} else if (offset != (offset & ~(chunksize - 1))) {
			offset &= ~(chunksize - 1);
			if (lseek(fd, offset, SEEK_SET) != offset) {
				ret = -EIO;
				goto err;
			}
		}

		if (offset > pos) {
			len = 0;
			do {
				*(void **)idx++ = &erofs_holechunk;
				pos += chunksize;
			} while (pos < offset);
			DBG_BUGON(pos != offset);
			lastch = NULL;
			continue;
		}
#endif

		len = min_t(u64, inode->i_size - pos, chunksize);
		ret = read(fd, chunkdata, len);
		if (ret < len) {
			ret = -EIO;
			goto err;
		}

		chunk = erofs_blob_getchunk(sbi, chunkdata, len);
		if (IS_ERR(chunk)) {
			ret = PTR_ERR(chunk);
			goto err;
		}

		if (lastch && (lastch->device_id != chunk->device_id ||
		    erofs_pos(sbi, lastch->blkaddr) + lastch->chunksize !=
		    erofs_pos(sbi, chunk->blkaddr))) {
			lb = lowbit(pos >> sbi->blkszbits);
			if (lb && lb < minextblks)
				minextblks = lb;
		}
		*(void **)idx++ = chunk;
		lastch = chunk;
	}
	inode->datalayout = EROFS_INODE_CHUNK_BASED;
	free(chunkdata);
	return erofs_blob_mergechunks(inode, chunkbits,
				      ilog2(minextblks) + sbi->blkszbits);
err:
	free(inode->chunkindexes);
	inode->chunkindexes = NULL;
	free(chunkdata);
	return ret;
}

int tarerofs_write_chunkes(struct erofs_inode *inode, erofs_off_t data_offset)
{
	struct erofs_sb_info *sbi = inode->sbi;
	unsigned int chunkbits = ilog2(inode->i_size - 1) + 1;
	unsigned int count, unit, device_id;
	erofs_off_t chunksize, len, pos;
	erofs_blk_t blkaddr;
	struct erofs_inode_chunk_index *idx;

	if (chunkbits < sbi->blkszbits)
		chunkbits = sbi->blkszbits;
	if (chunkbits - sbi->blkszbits > EROFS_CHUNK_FORMAT_BLKBITS_MASK)
		chunkbits = EROFS_CHUNK_FORMAT_BLKBITS_MASK + sbi->blkszbits;

	inode->u.chunkformat |= chunkbits - sbi->blkszbits;
	if (sbi->extra_devices) {
		device_id = 1;
		inode->u.chunkformat |= EROFS_CHUNK_FORMAT_INDEXES;
		unit = sizeof(struct erofs_inode_chunk_index);
		DBG_BUGON(erofs_blkoff(sbi, data_offset));
		blkaddr = erofs_blknr(sbi, data_offset);
	} else {
		device_id = 0;
		unit = EROFS_BLOCK_MAP_ENTRY_SIZE;
		DBG_BUGON(erofs_blkoff(sbi, datablob_size));
		blkaddr = erofs_blknr(sbi, datablob_size);
		datablob_size += round_up(inode->i_size, erofs_blksiz(sbi));
	}
	chunksize = 1ULL << chunkbits;
	count = DIV_ROUND_UP(inode->i_size, chunksize);

	inode->extent_isize = count * unit;
	idx = calloc(count, max(sizeof(*idx), sizeof(void *)));
	if (!idx)
		return -ENOMEM;
	inode->chunkindexes = idx;

	for (pos = 0; pos < inode->i_size; pos += len) {
		struct erofs_blobchunk *chunk;

		len = min_t(erofs_off_t, inode->i_size - pos, chunksize);

		chunk = erofs_get_unhashed_chunk(device_id, blkaddr,
						 data_offset);
		if (IS_ERR(chunk)) {
			free(inode->chunkindexes);
			inode->chunkindexes = NULL;
			return PTR_ERR(chunk);
		}

		*(void **)idx++ = chunk;
		blkaddr += erofs_blknr(sbi, len);
		data_offset += len;
	}
	inode->datalayout = EROFS_INODE_CHUNK_BASED;
	return 0;
}

int erofs_mkfs_dump_blobs(struct erofs_sb_info *sbi)
{
	struct erofs_buffer_head *bh;
	ssize_t length;
	erofs_off_t pos_in, pos_out;
	ssize_t ret;

	if (blobfile) {
		fflush(blobfile);
		length = ftell(blobfile);
		if (length < 0)
			return -errno;

		if (sbi->extra_devices)
			sbi->devs[0].blocks = erofs_blknr(sbi, length);
		else
			datablob_size = length;
	}

	if (sbi->extra_devices) {
		unsigned int i;

		pos_out = erofs_btell(bh_devt, false);
		i = 0;
		do {
			struct erofs_deviceslot dis = {
				.blocks = cpu_to_le32(sbi->devs[i].blocks),
			};
			int ret;

			ret = dev_write(sbi, &dis, pos_out, sizeof(dis));
			if (ret)
				return ret;
			pos_out += sizeof(dis);
		} while (++i < sbi->extra_devices);
		bh_devt->op = &erofs_drop_directly_bhops;
		erofs_bdrop(bh_devt, false);
		return 0;
	}

	bh = erofs_balloc(DATA, blobfile ? datablob_size : 0, 0, 0);
	if (IS_ERR(bh))
		return PTR_ERR(bh);

	erofs_mapbh(bh->block);

	pos_out = erofs_btell(bh, false);
	remapped_base = erofs_blknr(sbi, pos_out);
	if (blobfile) {
		pos_in = 0;
		ret = erofs_copy_file_range(fileno(blobfile), &pos_in,
				sbi->devfd, &pos_out, datablob_size);
		ret = ret < datablob_size ? -EIO : 0;
	} else {
		ret = 0;
	}
	bh->op = &erofs_drop_directly_bhops;
	erofs_bdrop(bh, false);
	return ret;
}

void erofs_blob_exit(void)
{
	struct hashmap_iter iter;
	struct hashmap_entry *e;
	struct erofs_blobchunk *bc, *n;

	if (blobfile)
		fclose(blobfile);

	while ((e = hashmap_iter_first(&blob_hashmap, &iter))) {
		bc = container_of((struct hashmap_entry *)e,
				  struct erofs_blobchunk, ent);
		DBG_BUGON(hashmap_remove(&blob_hashmap, e) != e);
		free(bc);
	}
	DBG_BUGON(hashmap_free(&blob_hashmap));

	list_for_each_entry_safe(bc, n, &unhashed_blobchunks, list) {
		list_del(&bc->list);
		free(bc);
	}
}

int erofs_blob_init(const char *blobfile_path)
{
	if (!blobfile_path) {
#ifdef HAVE_TMPFILE64
		blobfile = tmpfile64();
#else
		blobfile = tmpfile();
#endif
		multidev = false;
	} else {
		blobfile = fopen(blobfile_path, "wb");
		multidev = true;
	}
	if (!blobfile)
		return -EACCES;

	hashmap_init(&blob_hashmap, erofs_blob_hashmap_cmp, 0);
	return 0;
}

int erofs_mkfs_init_devices(struct erofs_sb_info *sbi, unsigned int devices)
{
	if (!devices)
		return 0;

	sbi->devs = calloc(devices, sizeof(sbi->devs[0]));
	if (!sbi->devs)
		return -ENOMEM;

	bh_devt = erofs_balloc(DEVT,
		sizeof(struct erofs_deviceslot) * devices, 0, 0);
	if (IS_ERR(bh_devt)) {
		free(sbi->devs);
		return PTR_ERR(bh_devt);
	}
	erofs_mapbh(bh_devt->block);
	bh_devt->op = &erofs_skip_write_bhops;
	sbi->devt_slotoff = erofs_btell(bh_devt, false) / EROFS_DEVT_SLOT_SIZE;
	sbi->extra_devices = devices;
	erofs_sb_set_device_table(sbi);
	return 0;
}
