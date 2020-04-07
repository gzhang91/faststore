
#ifndef _FS_SERVER_GLOBAL_H
#define _FS_SERVER_GLOBAL_H

#include "fastcommon/common_define.h"
#include "fastcommon/server_id_func.h"
#include "common/fs_cluster_cfg.h"
#include "sf/sf_global.h"
#include "server_types.h"

typedef struct server_global_vars {
    struct {
        FSClusterServerInfo *myself;
        struct {
            FSClusterConfig ctx;
            unsigned char md5_digest[16];
            int cluster_group_index;
            int service_group_index;
        } config;

        FSClusterServerArray server_array;

        SFContext sf_context;  //for cluster communication
    } cluster;

    struct {
        volatile uint64_t current_version; //binlog version
        string_t path;   //data path
        int binlog_buffer_size;
    } data;

    struct {
        int channels_between_two_servers;
    } replica;

} FSServerGlobalVars;

#define CLUSTER_CONFIG_CTX    g_server_global_vars.cluster.config.ctx
#define SERVER_CONFIG_CTX     g_server_global_vars.cluster.config.ctx.server_cfg

#define CLUSTER_MYSELF_PTR    g_server_global_vars.cluster.myself

#define CLUSTER_SERVER_ARRAY  g_server_global_vars.cluster.server_array

#define CLUSTER_MY_SERVER_ID  CLUSTER_MYSELF_PTR->server->id

#define CLUSTER_SF_CTX        g_server_global_vars.cluster.sf_context

#define BINLOG_BUFFER_SIZE    g_server_global_vars.data.binlog_buffer_size
#define DATA_CURRENT_VERSION  g_server_global_vars.data.current_version
#define DATA_PATH             g_server_global_vars.data.path
#define DATA_PATH_STR         DATA_PATH.str
#define DATA_PATH_LEN         DATA_PATH.len

#define REPLICA_CHANNELS_BETWEEN_TWO_SERVERS  \
    g_server_global_vars.replica.channels_between_two_servers

#define CLUSTER_GROUP_INDEX     g_server_global_vars.cluster.config.cluster_group_index
#define SERVICE_GROUP_INDEX     g_server_global_vars.cluster.config.service_group_index

#define CLUSTER_GROUP_ADDRESS_ARRAY(server) \
    (server)->group_addrs[CLUSTER_GROUP_INDEX].address_array
#define SERVICE_GROUP_ADDRESS_ARRAY(server) \
    (server)->group_addrs[SERVICE_GROUP_INDEX].address_array

#define CLUSTER_GROUP_ADDRESS_FIRST_PTR(server) \
    (*(server)->group_addrs[CLUSTER_GROUP_INDEX].address_array.addrs)
#define SERVICE_GROUP_ADDRESS_FIRST_PTR(server) \
    (*(server)->group_addrs[SERVICE_GROUP_INDEX].address_array.addrs)

#define CLUSTER_GROUP_ADDRESS_FIRST_IP(server) \
    CLUSTER_GROUP_ADDRESS_FIRST_PTR(server)->conn.ip_addr
#define CLUSTER_GROUP_ADDRESS_FIRST_PORT(server) \
    CLUSTER_GROUP_ADDRESS_FIRST_PTR(server)->conn.port

#define SERVICE_GROUP_ADDRESS_FIRST_IP(server) \
    SERVICE_GROUP_ADDRESS_FIRST_PTR(server)->conn.ip_addr
#define SERVICE_GROUP_ADDRESS_FIRST_PORT(server) \
    SERVICE_GROUP_ADDRESS_FIRST_PTR(server)->conn.port

#define CLUSTER_CONFIG_SIGN_BUF g_server_global_vars.cluster.config.md5_digest
#define CLUSTER_CONFIG_SIGN_LEN 16

#ifdef __cplusplus
extern "C" {
#endif

    extern FSServerGlobalVars g_server_global_vars;

#ifdef __cplusplus
}
#endif

#endif