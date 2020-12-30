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

//common_handler.c

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include "fastcommon/logger.h"
#include "fastcommon/sockopt.h"
#include "fastcommon/shared_func.h"
#include "fastcommon/pthread_func.h"
#include "fastcommon/sched_thread.h"
#include "sf/sf_func.h"
#include "sf/sf_nio.h"
#include "sf/sf_global.h"
#include "common/fs_proto.h"
#include "server_global.h"
#include "server_func.h"
#include "server_group_info.h"
#include "replication/replication_processor.h"
#include "replication/replication_common.h"
#include "cluster_topology.h"
#include "cluster_relationship.h"
#include "common_handler.h"

static int handler_check_config_sign(struct fast_task_info *task,
        const int server_id, const unsigned char *config_sign,
        const unsigned char *my_sign, const int sign_len,
        const char *caption)
{
    if (memcmp(config_sign, my_sign, sign_len) != 0) {
        char peer_hex[2 * CLUSTER_CONFIG_SIGN_LEN + 1];
        char my_hex[2 * CLUSTER_CONFIG_SIGN_LEN + 1];

        bin2hex((const char *)config_sign, sign_len, peer_hex);
        bin2hex((const char *)my_sign, sign_len, my_hex);

        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "server #%d 's %s config md5: %s != mine: %s",
                server_id, caption, peer_hex, my_hex);
        return EFAULT;
    }

    return 0;
}

int handler_check_config_signs(struct fast_task_info *task,
        const int server_id, FSProtoConfigSigns *config_signs)
{
    int result;
    if ((result=handler_check_config_sign(task, server_id,
                    config_signs->servers, SERVERS_CONFIG_SIGN_BUF,
                    SERVERS_CONFIG_SIGN_LEN, "servers")) != 0)
    {
        return result;
    }

    if ((result=handler_check_config_sign(task, server_id,
                    config_signs->cluster, CLUSTER_CONFIG_SIGN_BUF,
                    CLUSTER_CONFIG_SIGN_LEN, "cluster")) != 0)
    {
        return result;
    }

    return 0;
}

int handler_deal_task_done(struct fast_task_info *task)
{
    FSProtoHeader *proto_header;
    int r;
    int time_used;
    int log_level;
    char time_buff[32];

    if (TASK_ARG->context.log_level != LOG_NOTHING &&
            RESPONSE.error.length > 0)
    {
        log_it_ex(&g_log_context, TASK_ARG->context.log_level,
                "file: "__FILE__", line: %d, "
                "peer %s:%u, cmd: %d (%s), req body length: %d, %s",
                __LINE__, task->client_ip, task->port, REQUEST.header.cmd,
                fs_get_cmd_caption(REQUEST.header.cmd),
                REQUEST.header.body_len, RESPONSE.error.message);
    }

    if (!TASK_ARG->context.need_response) {
        time_used = (int)(get_current_time_us() - TASK_ARG->req_start_time);

        switch (REQUEST.header.cmd) {
            case SF_PROTO_ACTIVE_TEST_RESP:
                log_level = LOG_NOTHING;
                break;
            default:
                //log_level = LOG_INFO;
                log_level = LOG_DEBUG;
                break;
        }

        if (FC_LOG_BY_LEVEL(log_level)) {
            log_it_ex(&g_log_context, log_level, "file: "__FILE__", line: %d, "
                    "client %s:%u, req cmd: %d (%s), req body_len: %d, "
                    "status: %d, time used: %s us", __LINE__,
                    task->client_ip, task->port, REQUEST.header.cmd,
                    fs_get_cmd_caption(REQUEST.header.cmd),
                    REQUEST.header.body_len, RESPONSE_STATUS,
                    long_to_comma_str(time_used, time_buff));
        }

        if (RESPONSE_STATUS == 0) {
            task->offset = task->length = 0;
            return sf_set_read_event(task);
        }
        return RESPONSE_STATUS > 0 ? -1 * RESPONSE_STATUS : RESPONSE_STATUS;
    }

    proto_header = (FSProtoHeader *)task->data;
    if (!TASK_ARG->context.response_done) {
        RESPONSE.header.body_len = RESPONSE.error.length;
        if (RESPONSE.error.length > 0) {
            memcpy(task->data + sizeof(FSProtoHeader),
                    RESPONSE.error.message, RESPONSE.error.length);
        }
    }

    short2buff(RESPONSE_STATUS >= 0 ? RESPONSE_STATUS : -1 * RESPONSE_STATUS,
            proto_header->status);
    proto_header->cmd = RESPONSE.header.cmd;
    int2buff(RESPONSE.header.body_len, proto_header->body_len);
    task->length = sizeof(FSProtoHeader) + RESPONSE.header.body_len;

    r = sf_send_add_event(task);
    time_used = (int)(get_current_time_us() - TASK_ARG->req_start_time);
    if (SLOW_LOG_CFG.enabled && time_used >
            SLOW_LOG_CFG.log_slower_than_ms * 1000)
    {
        char buff[256];
        int blen;

        blen = sprintf(buff, "timed used: %s us, client %s:%u, "
                "req cmd: %d (%s), req body len: %d, resp cmd: %d (%s), "
                "status: %d, resp body len: %d", long_to_comma_str(time_used,
                    time_buff), task->client_ip, task->port,
                REQUEST.header.cmd, fs_get_cmd_caption(REQUEST.header.cmd),
                REQUEST.header.body_len, RESPONSE.header.cmd,
                fs_get_cmd_caption(RESPONSE.header.cmd),
                RESPONSE_STATUS, RESPONSE.header.body_len);
        log_it_ex2(&SLOW_LOG_CTX, NULL, buff, blen, false, true);
    }

    switch (REQUEST.header.cmd) {
        case FS_CLUSTER_PROTO_PING_LEADER_REQ:
        case SF_PROTO_ACTIVE_TEST_REQ:
            log_level = LOG_NOTHING;
            break;
        default:
            //log_level = LOG_INFO;
            log_level = LOG_DEBUG;
            //log_level = RESPONSE_STATUS == 0 ? LOG_DEBUG : LOG_WARNING;
            break;
    }

    if (FC_LOG_BY_LEVEL(log_level)) {
        log_it_ex(&g_log_context, log_level, "file: "__FILE__", line: %d, "
                "client %s:%u, req cmd: %d (%s), req body_len: %d, "
                "resp cmd: %d (%s), status: %d, resp body_len: %d, "
                "time used: %s us", __LINE__,
                task->client_ip, task->port, REQUEST.header.cmd,
                fs_get_cmd_caption(REQUEST.header.cmd),
                REQUEST.header.body_len, RESPONSE.header.cmd,
                fs_get_cmd_caption(RESPONSE.header.cmd),
                RESPONSE_STATUS, RESPONSE.header.body_len,
                long_to_comma_str(time_used, time_buff));
    }

    return r == 0 ? RESPONSE_STATUS : r;
}
