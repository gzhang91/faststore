#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "fastcommon/logger.h"
#include "fastcommon/process_ctrl.h"
#include "fastcommon/sched_thread.h"
#include "sf/sf_global.h"
#include "sf/sf_service.h"
#include "sf/sf_util.h"
#include "fs_fuse_global.h"
#include "fs_fuse_wrapper.h"

static bool daemon_mode = true;
static int setup_server_env(const char *config_filename);
static struct fuse_session *create_fuse_session(char *argv0,
        struct fuse_lowlevel_ops *ops);

int main(int argc, char *argv[])
{
    char *config_filename;
    char *action;
    char pid_filename[MAX_PATH_SIZE];
    pthread_t schedule_tid;
    bool stop;
    int wait_count;
	struct fuse_lowlevel_ops fuse_operations;
	struct fuse_session *se;
	int result;

        stop = false;
    if (argc < 2) {
        sf_usage(argv[0]);
        return 1;
    }

    config_filename = argv[1];
    log_init2();
    //log_set_time_precision(&g_log_context, LOG_TIME_PRECISION_USECOND);

    result = get_base_path_from_conf_file(config_filename,
            SF_G_BASE_PATH, sizeof(SF_G_BASE_PATH));
    if (result != 0) {
        log_destroy();
        return result;
    }

    snprintf(pid_filename, sizeof(pid_filename),
             "%s/serverd.pid", SF_G_BASE_PATH);

    sf_parse_daemon_mode_and_action(argc, argv, &daemon_mode, &action);
    result = process_action(pid_filename, action, &stop);
    if (result != 0) {
        if (result == EINVAL) {
            sf_usage(argv[0]);
        }
        log_destroy();
        return result;
    }

    if (stop) {
        log_destroy();
        return 0;
    }

    do {
        if ((result=setup_server_env(config_filename)) != 0) {
            break;
        }

        if ((result=fs_fuse_wrapper_init(&fuse_operations)) != 0) {
            break;
        }

        if ((result=sf_startup_schedule(&schedule_tid)) != 0) {
            break;
        }

        if ((se=create_fuse_session(argv[0], &fuse_operations)) == NULL) {
            result = ENOMEM;
            break;
        }

        if ((result=fuse_set_signal_handlers(se)) != 0) {
            break;
        }

        if ((result=fuse_session_mount(se, g_fuse_global_vars.
                        mountpoint)) != 0)
        {
            break;
        }

        /* Block until ctrl+c or fusermount -u */
        if (g_fuse_global_vars.singlethread) {
            result = fuse_session_loop(se);
        } else {
            struct fuse_loop_config fuse_config;
            fuse_config.clone_fd = g_fuse_global_vars.clone_fd;
            fuse_config.max_idle_threads = g_fuse_global_vars.max_idle_threads;
            result = fuse_session_loop_mt(se, &fuse_config);
        }

        fuse_session_unmount(se);
    } while (0);

    if (g_schedule_flag) {
        pthread_kill(schedule_tid, SIGINT);
    }

    wait_count = 0;
    while (g_schedule_flag) {
        usleep(10000);
        if (++wait_count > 1000) {
            lwarning("waiting timeout, exit!");
            break;
        }
    }

    delete_pid_file(pid_filename);
    if (result == 0) {
        logInfo("file: "__FILE__", line: %d, "
                "program exit normally.\n", __LINE__);
    }
    log_destroy();

	return result < 0 ? 1 : result;
}

static int setup_server_env(const char *config_filename)
{
    int result;

    sf_set_current_time();
    if ((result=fs_fuse_global_init(config_filename)) != 0) {
        return result;
    }

    if (daemon_mode) {
        daemon_init(false);
    }
    umask(0);

    result = sf_setup_signal_handler();

    log_set_cache(true);
    return result;
}

static struct fuse_session *create_fuse_session(char *argv0,
        struct fuse_lowlevel_ops *ops)
{
	struct fuse_args args;
    char *argv[8];
    int argc;

    argc = 0;
    argv[argc++] = argv0;

    if (g_log_context.log_level == LOG_DEBUG) {
        argv[argc++] = "-d";
    }

    if (g_fuse_global_vars.auto_unmount) {
        argv[argc++] = "-o";
        argv[argc++] = "auto_unmount";
    }

    if (g_fuse_global_vars.allow_others == allow_root) {
        argv[argc++] = "-o";
        argv[argc++] = "allow_root";
    } else if (g_fuse_global_vars.allow_others == allow_all) {
        argv[argc++] = "-o";
        argv[argc++] = "allow_other";
    }

    args.argc = argc;
    args.argv = argv;
    args.allocated = 0;
    return fuse_session_new(&args, ops, sizeof(*ops), NULL);
}
