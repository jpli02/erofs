/* SPDX-License-Identifier: GPL-2.0+ OR Apache-2.0 */
/*
 * Copyright (C) 2019 HUAWEI, Inc.
 *             http://www.huawei.com/
 * Created by Gao Xiang <gaoxiang25@huawei.com>
 */
#ifndef __EROFS_COMPRESS_H
#define __EROFS_COMPRESS_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "internal.h"

#define EROFS_CONFIG_COMPR_MAX_SZ           (4000 * 1024)

void z_erofs_drop_inline_pcluster(struct erofs_inode *inode);
int erofs_write_compressed_file(struct erofs_inode *inode, int fd);

int z_erofs_compress_init(struct erofs_buffer_head *bh);
int z_erofs_compress_exit(void);

const char *z_erofs_list_available_compressors(unsigned int i);

static inline bool erofs_is_packed_inode(struct erofs_inode *inode)
{
	if (inode->nid == EROFS_PACKED_NID_UNALLOCATED) {
		DBG_BUGON(sbi.packed_nid != EROFS_PACKED_NID_UNALLOCATED);
		return true;
	}
	return (sbi.packed_nid > 0 && inode->nid == sbi.packed_nid);
}

#ifdef __cplusplus
}
#endif

#endif
