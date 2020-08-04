#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <pthread.h>
#include "fastcommon/shared_func.h"
#include "fastcommon/logger.h"
#include "fastcommon/sched_thread.h"
#include "fastcommon/thread_pool.h"
#include "sf/sf_global.h"
#include "../../common/fs_proto.h"
#include "../../common/fs_func.h"
#include "../server_global.h"
#include "../cluster_relationship.h"
#include "../server_binlog.h"
#include "../server_replication.h"
#include "../server_storage.h"
#include "data_recovery.h"
#include "binlog_replay.h"

#define FIXED_THREAD_CONTEXT_COUNT  16

struct binlog_replay_context;
struct replay_thread_context;

typedef struct replay_task_info {
    int op_type;
    FSSliceOpContext op_ctx;
    struct replay_thread_context *thread_ctx;
    struct replay_task_info *next;
} ReplayTaskInfo;

typedef struct replay_thread_context {
    struct {
        struct fc_queue freelist; //element: ReplayTaskInfo
        struct fc_queue waiting;  //element: ReplayTaskInfo
    } queues;

    struct {
        pthread_mutex_t lock;
        pthread_cond_t cond;
    } notify;

    struct {
        int64_t write_count;
        int64_t allocate_count;
        int64_t delete_count;
        int64_t fail_count;
    } stat;

    struct ob_slice_ptr_array slice_ptr_array;
    struct binlog_replay_context *replay_ctx;
} ReplayThreadContext;

typedef struct binlog_replay_context {
    volatile int running_count;
    bool continue_flag;
    BinlogReadThreadContext rdthread_ctx;
    BinlogReadThreadResult *r;
    struct {
        ReplayThreadContext *contexts;
        ReplayThreadContext fixed[FIXED_THREAD_CONTEXT_COUNT];
        ReplayTaskInfo *tasks;   //holder
    } thread_env;
    ReplicaBinlogRecord record;
} BinlogReplayContext;

static FCThreadPool replay_thread_pool;

int binlog_replay_init()
{
    int result;
    int limit;
    const int max_idle_time = 60;
    const int min_idle_count = 0;

    limit = DATA_RECOVERY_THREADS_LIMIT * RECOVERY_THREADS_PER_DATA_GROUP;
    if ((result=fc_thread_pool_init(&replay_thread_pool,
                    limit, SF_G_THREAD_STACK_SIZE, max_idle_time,
                    min_idle_count, (bool *)&SF_G_CONTINUE_FLAG)) != 0)
    {
        return result;
    }

    return 0;
}

void binlog_replay_destroy()
{
}

static void slice_write_done_notify(FSSliceOpContext *op_ctx)
{
    ReplayThreadContext *thread_ctx;

    thread_ctx = (ReplayThreadContext *)op_ctx->notify.arg;
    pthread_cond_signal(&thread_ctx->notify.cond);
}

static int deal_task(ReplayTaskInfo *task)
{
    int result;
    char *buff;
    int inc_alloc;
    int dec_alloc;

    switch (task->op_type) {
        case REPLICA_BINLOG_OP_TYPE_DEL_SLICE:
            result = fs_delete_slices(&task->op_ctx, &dec_alloc);
            break;
        case REPLICA_BINLOG_OP_TYPE_WRITE_SLICE:
            //TODO
            buff = NULL;
            result = fs_slice_write(&task->op_ctx, buff);
            break;
        case REPLICA_BINLOG_OP_TYPE_ALLOC_SLICE:
            result = fs_slice_allocate_ex(&task->op_ctx,
                    &task->thread_ctx->slice_ptr_array, &inc_alloc);
            break;
        default:
            logError("file: "__FILE__", line: %d, "
                    "unkown op type: %c (0x%02x)",
                    __LINE__, task->op_type, task->op_type);
            return EINVAL;
    }

    return result;
}

static void binlog_replay_run(void *arg)
{
    ReplayThreadContext *thread_ctx;
    ReplayTaskInfo *task;

    thread_ctx = (ReplayThreadContext *)arg;
    __sync_add_and_fetch(&thread_ctx->replay_ctx->running_count, 1);
    while (thread_ctx->replay_ctx->continue_flag) {
        if ((task=(ReplayTaskInfo *)fc_queue_try_pop(
                        &thread_ctx->queues.waiting)) == NULL)
        {
            usleep(100000);
            continue;
        }

        do {
            if (deal_task(task) != 0) {
                break;
            }
            fc_queue_push(&thread_ctx->queues.freelist, task);
            task = (ReplayTaskInfo *)fc_queue_try_pop(
                    &thread_ctx->queues.waiting);
        } while (task != NULL && SF_G_CONTINUE_FLAG);
    }

    __sync_sub_and_fetch(&thread_ctx->replay_ctx->running_count, 1);
}

static int deal_binlog_buffer(DataRecoveryContext *ctx)
{
    BinlogReplayContext *replay_ctx;
    ReplayThreadContext *thread_ctx;
    ReplayTaskInfo *task;
    char *p;
    char *line_end;
    char *end;
    BufferInfo *buffer;
    string_t line;
    char error_info[256];
    int result;
    int op_type;

    replay_ctx = (BinlogReplayContext *)ctx->arg;
    result = 0;
    *error_info = '\0';
    buffer = &replay_ctx->r->buffer;
    end = buffer->buff + buffer->length;
    p = buffer->buff;
    while (p < end) {
        line_end = (char *)memchr(p, '\n', end - p);
        if (line_end == NULL) {
            strcpy(error_info, "expect end line (\\n)");
            result = EINVAL;
            break;
        }

        line_end++;
        line.str = p;
        line.len = line_end - p;
        if ((result=replica_binlog_record_unpack(&line,
                        &replay_ctx->record, error_info)) != 0)
        {
            break;
        }

        op_type = replay_ctx->record.op_type;
        fs_calc_block_hashcode(&replay_ctx->record.bs_key.block);

        thread_ctx = replay_ctx->thread_env.contexts +
            FS_BLOCK_HASH_CODE(replay_ctx->record.bs_key.block) %
            RECOVERY_THREADS_PER_DATA_GROUP;
        while (1) {
            if ((task=(ReplayTaskInfo *)fc_queue_pop(
                            &thread_ctx->queues.freelist)) != NULL)
            {
                break;
            }

            if (!SF_G_CONTINUE_FLAG) {
                return EINTR;
            }
        }

        task->op_type = replay_ctx->record.op_type;
        task->op_ctx.info.data_version = replay_ctx->record.data_version;
        task->op_ctx.info.bs_key = replay_ctx->record.bs_key;
        fc_queue_push(&thread_ctx->queues.waiting, task);

        p = line_end;
    }

    if (result != 0) {
        ServerBinlogReader *reader;
        int64_t offset;
        int64_t line_count;

        reader = &replay_ctx->rdthread_ctx.reader;
        offset = reader->position.offset + (p - buffer->buff);
        fc_get_file_line_count_ex(reader->filename, offset, &line_count);

        logError("file: "__FILE__", line: %d, "
                "binlog file %s, line no: %"PRId64", %s",
                __LINE__, reader->filename,
                line_count + 1, error_info);
    }

    return result;
}

static int do_replay_binlog(DataRecoveryContext *ctx)
{
    BinlogReplayContext *replay_ctx;
    char subdir_name[FS_BINLOG_SUBDIR_NAME_SIZE];
    int result;

    replay_ctx = (BinlogReplayContext *)ctx->arg;
    data_recovery_get_subdir_name(ctx, RECOVERY_BINLOG_SUBDIR_NAME_REPLAY,
            subdir_name);
    if ((result=binlog_read_thread_init(&replay_ctx->rdthread_ctx, subdir_name,
                    NULL, NULL, BINLOG_BUFFER_SIZE)) != 0)
    {
        return result;
    }

    logInfo("file: "__FILE__", line: %d, "
            "dedup %s data ...", __LINE__, subdir_name);

    result = 0;
    while (SF_G_CONTINUE_FLAG) {
        if ((replay_ctx->r=binlog_read_thread_fetch_result(
                        &replay_ctx->rdthread_ctx)) == NULL)
        {
            result = EINTR;
            break;
        }

        logInfo("errno: %d, buffer length: %d", replay_ctx->r->err_no,
                replay_ctx->r->buffer.length);
        if (replay_ctx->r->err_no == ENOENT) {
            break;
        } else if (replay_ctx->r->err_no != 0) {
            result = replay_ctx->r->err_no;
            break;
        }

        if ((result=deal_binlog_buffer(ctx)) != 0) {
            break;
        }

        binlog_read_thread_return_result_buffer(&replay_ctx->rdthread_ctx,
                replay_ctx->r);
    }

    binlog_read_thread_terminate(&replay_ctx->rdthread_ctx);

    if (result != 0) {
        replay_ctx->continue_flag = false;
    } else {
        usleep(100000);
    }

    while (__sync_add_and_fetch(&replay_ctx->running_count, 0) > 0) {
        usleep(100000);
        logInfo("data group id: %d, replay running threads: %d",
                ctx->data_group_id, __sync_add_and_fetch(
                    &replay_ctx->running_count, 0));
    }

    return result;
}

static int init_rthread_context(ReplayThreadContext *thread_ctx,
        ReplayTaskInfo *tasks)
{
    int result;
    bool notify;
    ReplayTaskInfo *task;
    ReplayTaskInfo *end;

    if ((result=fc_queue_init(&thread_ctx->queues.freelist, (long)
                    (&((ReplayTaskInfo *)NULL)->next))) != 0)
    {
        return result;
    }

    if ((result=fc_queue_init(&thread_ctx->queues.waiting, (long)
                    (&((ReplayTaskInfo *)NULL)->next))) != 0)
    {
        return result;
    }

    if ((result=init_pthread_lock(&thread_ctx->notify.lock)) != 0) {
        return result;
    }

    if ((result=pthread_cond_init(&thread_ctx->notify.cond, NULL)) != 0) {
        logError("file: "__FILE__", line: %d, "
                "pthread_cond_init fail, "
                "errno: %d, error info: %s",
                __LINE__, result, STRERROR(result));
        return result;
    }

    end = tasks + RECOVERY_MAX_QUEUE_DEPTH;
    for (task=tasks; task<end; task++) {
        task->op_ctx.notify.func = slice_write_done_notify;
        task->op_ctx.notify.arg = thread_ctx;
        task->thread_ctx = thread_ctx;
        fc_queue_push_ex(&thread_ctx->queues.freelist, task, &notify);
    }

    ob_index_init_slice_ptr_array(&thread_ctx->slice_ptr_array);
    return 0;
}

static int init_replay_tasks(DataRecoveryContext *ctx)
{
    BinlogReplayContext *replay_ctx;
    ReplayTaskInfo *task;
    ReplayTaskInfo *end;
    int count;
    int bytes;

    replay_ctx = (BinlogReplayContext *)ctx->arg;
    count = RECOVERY_THREADS_PER_DATA_GROUP * RECOVERY_MAX_QUEUE_DEPTH;
    bytes = sizeof(ReplayTaskInfo) * count;
    replay_ctx->thread_env.tasks = (ReplayTaskInfo *)fc_malloc(bytes);
    if (replay_ctx->thread_env.tasks == NULL) {
        return ENOMEM;
    }

    end = replay_ctx->thread_env.tasks + count;
    for (task=replay_ctx->thread_env.tasks; task<end; task++) {
        task->op_ctx.info.write_data_binlog = true;
        task->op_ctx.info.data_group_id = ctx->data_group_id;
        task->op_ctx.info.myself = ctx->master->dg->myself;
    }

    return 0;
}

static int int_replay_context(DataRecoveryContext *ctx)
{
    BinlogReplayContext *replay_ctx;
    ReplayThreadContext *context;
    ReplayThreadContext *end;
    ReplayTaskInfo *tasks;
    int bytes;
    int result;

    replay_ctx = (BinlogReplayContext *)ctx->arg;
    bytes = sizeof(ReplayThreadContext) * RECOVERY_THREADS_PER_DATA_GROUP;
    if (RECOVERY_THREADS_PER_DATA_GROUP <= FIXED_THREAD_CONTEXT_COUNT) {
        replay_ctx->thread_env.contexts = replay_ctx->thread_env.fixed;
    } else {
        replay_ctx->thread_env.contexts = (ReplayThreadContext *)
            fc_malloc(bytes);
        if (replay_ctx->thread_env.contexts == NULL) {
            return ENOMEM;
        }
    }
    memset(replay_ctx->thread_env.contexts, 0, bytes);

    if ((result=init_replay_tasks(ctx)) != 0) {
        return result;
    }

    replay_ctx->continue_flag = true;
    end = replay_ctx->thread_env.contexts + RECOVERY_THREADS_PER_DATA_GROUP;
    for (context=replay_ctx->thread_env.contexts,
            tasks=replay_ctx->thread_env.tasks; context<end;
            context++, tasks += RECOVERY_MAX_QUEUE_DEPTH)
    {
        context->replay_ctx = replay_ctx;
        if ((result=init_rthread_context(context, tasks)) != 0) {
            break;
        }

        if ((result=fc_thread_pool_run(&replay_thread_pool,
                        binlog_replay_run, context)) != 0)
        {
            break;
        }
    }

    if (result != 0) {
        replay_ctx->continue_flag = false;
    }

    return result;
}

static void destroy_replay_context(BinlogReplayContext *replay_ctx)
{
    ReplayThreadContext *context;
    ReplayThreadContext *end;

    end = replay_ctx->thread_env.contexts + RECOVERY_THREADS_PER_DATA_GROUP;
    for (context=replay_ctx->thread_env.contexts; context<end; context++) {
        fc_queue_destroy(&context->queues.freelist);
        fc_queue_destroy(&context->queues.waiting);

        pthread_cond_destroy(&context->notify.cond);
        pthread_mutex_destroy(&context->notify.lock);

        ob_index_free_slice_ptr_array(&context->slice_ptr_array);
    }
    free(replay_ctx->thread_env.tasks);

    if (replay_ctx->thread_env.contexts != replay_ctx->thread_env.fixed) {
        free(replay_ctx->thread_env.contexts);
    }
}

int data_recovery_replay_binlog(DataRecoveryContext *ctx)
{
    int result;
    BinlogReplayContext replay_ctx;

    ctx->arg = &replay_ctx;
    memset(&replay_ctx, 0, sizeof(replay_ctx));

    if ((result=int_replay_context(ctx)) != 0) {
        return result;
    }
    result = do_replay_binlog(ctx);
    destroy_replay_context(&replay_ctx);
    return result;
}