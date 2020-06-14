
#ifndef _FS_CLIENT_PROTO_H
#define _FS_CLIENT_PROTO_H

#include "fastcommon/fast_mpool.h"
#include "fs_types.h"
#include "fs_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

    int fs_client_proto_slice_write(FSClientContext *client_ctx,
            const FSBlockSliceKeyInfo *bs_key, const char *buff,
            int *write_bytes, int *inc_alloc);

    int fs_client_proto_slice_read(FSClientContext *client_ctx,
            const FSBlockSliceKeyInfo *bs_key, char *buff, int *read_bytes);

    int fs_client_proto_slice_allocate(FSClientContext *client_ctx,
            const FSBlockSliceKeyInfo *bs_key, int *inc_alloc);

    int fs_client_proto_slice_delete(FSClientContext *client_ctx,
            const FSBlockSliceKeyInfo *bs_key, int *dec_alloc);

    int fs_client_proto_block_delete(FSClientContext *client_ctx,
            const FSBlockKey *bkey, int *dec_alloc);

    int fs_client_proto_join_server(ConnectionInfo *conn,
            FSConnectionParameters *conn_params);

#ifdef __cplusplus
}
#endif

#endif
