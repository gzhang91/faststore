
#ifndef _OBJECT_BLOCK_INDEX_H
#define _OBJECT_BLOCK_INDEX_H

#include "fastcommon/fc_list.h"
#include "fastcommon/uniq_skiplist.h"
#include "../../common/fs_types.h"

typedef struct {
    UniqSkiplistFactory factory;
    struct fast_mblock_man ob_allocator;    //for ob_entry
    struct fast_mblock_man slice_allocator; //for slice_entry 
    pthread_mutex_t lock;
} OBSharedContext;

typedef struct ob_entry {
    FSBlockKey bkey;
    UniqSkiplist *slices;   //the element is OBSliceEntry
    struct ob_entry *next; //for hashtable
} OBEntry;

typedef struct ob_slice_entry {
    OBEntry *ob;
    volatile int ref_count;
    FSSliceSize ssize;
    FSTrunkSpaceInfo space;
    struct fc_list_head dlink;  //used in trunk entry for trunk reclaiming
} OBSliceEntry;

typedef struct ob_slice_ptr_array {
    int alloc;
    int count;
    OBSliceEntry **slices;
    OBSharedContext *ctx;
} OBSlicePtrArray;

#ifdef __cplusplus
extern "C" {
#endif

    int ob_index_init();
    void ob_index_destroy();

    int ob_index_add_slice(OBSliceEntry *slice);

    int ob_index_get_slices(const FSBlockSliceKeyInfo *bs_key,
            OBSlicePtrArray *sarray);

    OBSliceEntry *ob_index_alloc_slice(const FSBlockKey *bkey);

    void ob_index_free_slice(OBSliceEntry *slice);

    static inline void ob_index_init_slice_ptr_array(OBSlicePtrArray *sarray)
    {
        sarray->slices = NULL;
        sarray->alloc = sarray->count = 0;
    }

    static inline void ob_index_free_slice_ptr_array(OBSlicePtrArray *sarray)
    {
        if (sarray->slices != NULL) {
            free(sarray->slices);
            sarray->slices = NULL;
            sarray->alloc = sarray->count = 0;
        }
    }

#ifdef __cplusplus
}
#endif

#endif