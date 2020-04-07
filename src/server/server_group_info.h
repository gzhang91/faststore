//server_group_info.h

#ifndef _SERVER_GROUP_INFO_H_
#define _SERVER_GROUP_INFO_H_

#include <time.h>
#include <pthread.h>
#include "server_global.h"

#ifdef __cplusplus
extern "C" {
#endif

int server_group_info_init(const char *cluster_config_filename);
int server_group_info_destroy();

FSClusterServerInfo *fs_get_server_by_id(const int server_id);

int server_group_info_setup_sync_to_file_task();

static inline void server_group_info_set_status(FSClusterServerInfo *cs,
        const int status)
{
    if (cs->status != status) {
        cs->status = status;
        __sync_add_and_fetch(&CLUSTER_SERVER_ARRAY.change_version, 1);
    }
}

#ifdef __cplusplus
}
#endif

#endif