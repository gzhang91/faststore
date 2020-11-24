/*
 * Copyright (c) 2020 YuQing <384681@qq.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef _OPID_HTABLE_H
#define _OPID_HTABLE_H

#include "fs_api_types.h"
#include "sharding_htable.h"

typedef struct fs_api_otid_entry {
    FSAPIHashEntry hentry;  //must be the first
    int successive_count;
    int64_t last_write_offset;
    FSAPISliceEntry *slice;         //current combined slice
} FSAPIOTIDEntry;

#ifdef __cplusplus
extern "C" {
#endif

    int otid_htable_init(const int sharding_count,
            const int64_t htable_capacity,
            const int allocator_count, int64_t element_limit,
            const int64_t min_ttl_ms, const int64_t max_ttl_ms);

    int otid_htable_insert(FSAPIOperationContext *op_ctx,
            const char *buff, bool *combined);

    static inline void otid_htable_release_slice(FSAPISliceEntry *slice)
    {
        PTHREAD_MUTEX_LOCK(&slice->block->hentry.sharding->lock);
        fc_list_del_init(&slice->dlink);
        PTHREAD_MUTEX_UNLOCK(&slice->block->hentry.sharding->lock);

        PTHREAD_MUTEX_LOCK(&slice->otid->hentry.sharding->lock);
        slice->otid->slice = NULL;
        PTHREAD_MUTEX_UNLOCK(&slice->otid->hentry.sharding->lock);

        fast_mblock_free_object(slice->allocator, slice);
    }

#ifdef __cplusplus
}
#endif

#endif
