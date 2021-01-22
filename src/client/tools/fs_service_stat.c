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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "fastcommon/logger.h"
#include "faststore/client/fs_client.h"

static void usage(char *argv[])
{
    fprintf(stderr, "Usage: %s [-c config_filename] "
            "[-s server_id] [-g data_group_id=0] "
            "host[:port]\n", argv[0]);
}

static void output(FSClientServiceStat *stat)
{
    printf( "\tserver_id: %d\n"
            "\tis_leader: %s\n"
            "\tconnection : {current: %d, max: %d}\n"
            "\tbinlog : {current_version: %"PRId64", "
            "writer: {next_version: %"PRId64", total_count: %"PRId64", "
            "waiting_count: %d, max_waitings: %d}}\n\n",
            stat->server_id, stat->is_leader ? "true" : "false",
            stat->connection.current_count,
            stat->connection.max_count,
            stat->binlog.current_version,
            stat->binlog.writer.next_version,
            stat->binlog.writer.total_count,
            stat->binlog.writer.waiting_count,
            stat->binlog.writer.max_waitings
          );
}

int main(int argc, char *argv[])
{
    const char *config_filename = "/etc/fastcfs/fdir/client.conf";
	int ch;
    int server_id;
    int data_group_id;
    char *host;
    FCServerInfo *server;
    ConnectionInfo *spec_conn;
    ConnectionInfo conn;
    FSClientServiceStat stat;
	int result;

    if (argc < 2) {
        usage(argv);
        return 1;
    }

    server_id = 0;
    data_group_id = 0;
    while ((ch=getopt(argc, argv, "hc:s:g:")) != -1) {
        switch (ch) {
            case 'h':
                usage(argv);
                break;
            case 'c':
                config_filename = optarg;
                break;
            case 's':
                server_id = strtol(optarg, NULL, 10);
                break;
            case 'g':
                data_group_id = strtol(optarg, NULL, 10);
                break;
            default:
                usage(argv);
                return 1;
        }
    }

    if (server_id > 0) {
        spec_conn = NULL;
    } else {
        if (optind >= argc) {
            usage(argv);
            return 1;
        }

        host = argv[optind];
        if ((result=conn_pool_parse_server_info(host, &conn,
                        FS_SERVER_DEFAULT_SERVICE_PORT)) != 0)
        {
            return result;
        }
        spec_conn = &conn;
    }

    log_init();
    //g_log_context.log_level = LOG_DEBUG;

    if ((result=fs_client_init(config_filename)) != 0) {
        return result;
    }

    if (spec_conn == NULL) {
        FCAddressPtrArray *addr_parray;

        if ((server=fc_server_get_by_id(&g_fs_client_vars.client_ctx.
                        cluster_cfg.ptr->server_cfg, server_id)) == NULL)
        {
            logError("file: "__FILE__", line: %d, "
                    "server id: %d not exist",
                    __LINE__, server_id);
            return ENOENT;
        }

        addr_parray = &FS_CFG_SERVICE_ADDRESS_ARRAY(
                &g_fs_client_vars.client_ctx, server);
        spec_conn = &addr_parray->addrs[0]->conn;
    }

    if ((result=fs_client_proto_service_stat(&g_fs_client_vars.
                    client_ctx, spec_conn, data_group_id, &stat)) != 0)
    {
        return result;
    }

    output(&stat);
    return 0;
}