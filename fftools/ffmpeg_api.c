/*
 * Embeddable engine API – per-task callbacks and worker-thread helpers.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "config.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>

#include "libavutil/avstring.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavformat/avformat.h"

#if CONFIG_AVDEVICE
#include "libavdevice/avdevice.h"
#endif

#include "cmdutils.h"
#include "ffmpeg.h"
#include "ffmpeg_api.h"

/* ------------------------------------------------------------------------- */
/* Process-wide one-shot initialization (network, devices, dynload).         */
/* ------------------------------------------------------------------------- */

static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;

static void engine_global_init(void)
{
    init_dynload();
#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
    avformat_network_init();
}

/* ------------------------------------------------------------------------- */
/* Concurrency model:                                                        */
/*                                                                           */
/*   Most engine state lives in _Thread_local globals (input/output tables,  */
/*   filtergraphs, decoders, vstats, progress_avio, ...). A handful of CLI   */
/*   option variables are referenced by their addresses from a static       */
/*   options[] table (e.g. { &print_stats }) and therefore must remain at    */
/*   process scope.                                                          */
/*                                                                           */
/*   We use a *fine-grained* lock: ffmpeg.c serializes only the              */
/*   parse_options() phase via g_options_mutex (declared in ffmpeg_opt.c)    */
/*   and immediately snapshots those globals into thread-local copies via   */
/*   ffmpeg_snapshot_opts_to_tls(). After that the transcode loop runs      */
/*   completely lock-free across worker threads – two ffmpeg_run() calls    */
/*   thus execute their transcode pipelines in parallel.                    */
/*                                                                           */
/*   This file therefore does NOT take any extra mutex; the cross-task      */
/*   contention point is owned by ffmpeg.c.                                 */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* FFmpegTask                                                                */
/* ------------------------------------------------------------------------- */

struct FFmpegTask {
    int                  argc;
    char               **argv;
    int                  argv_owned;   /* if 1, free argv strings on destroy */

    int                  task_id;
    void                *user_data;

    ffmpeg_progress_cb   on_progress;
    ffmpeg_error_cb      on_error;
    ffmpeg_finish_cb     on_finish;

    /* Cooperative cancel flag, polled from the worker. */
    atomic_int           cancel_flag;

    /* Threading. */
    pthread_t            thread;
    int                  thread_started;
    int                  thread_joined;

    /* Result. */
    int                  ret_code;

    /* Set to 1 the first time on_error fires so we don't spam the user. */
    atomic_int           error_emitted;
};

/* Per-thread "current task" pointer. Reads/writes from inside ffmpeg.c. */
static _Thread_local FFmpegTask *tls_current_task = NULL;

FFmpegTask *ffmpeg_task_current(void)
{
    return tls_current_task;
}

void ffmpeg_task_set_current(FFmpegTask *task)
{
    tls_current_task = task;
}

int ffmpeg_task_is_cancelled(const FFmpegTask *task)
{
    return task ? atomic_load(&((FFmpegTask *)task)->cancel_flag) : 0;
}

void ffmpeg_task_emit_progress(const FFmpegProgress *p)
{
    FFmpegTask *t = tls_current_task;
    if (t && t->on_progress && p) {
        FFmpegProgress copy = *p;
        copy.task_id = t->task_id;
        t->on_progress(t, &copy, t->user_data);
    }
}

void ffmpeg_task_emit_error(int errcode, const char *msg)
{
    FFmpegTask *t;
    int expected = 0;

    t = tls_current_task;
    if (!t || !t->on_error)
        return;
    if (atomic_compare_exchange_strong(&t->error_emitted, &expected, 1))
        t->on_error(t, errcode, msg ? msg : "", t->user_data);
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

FFmpegTask *ffmpeg_task_alloc(int argc, char **argv)
{
    FFmpegTask *t;

    if (argc <= 0 || !argv)
        return NULL;

    t = av_mallocz(sizeof(*t));
    if (!t)
        return NULL;

    /* Deep-copy argv so the caller is free to release its own buffer
     * immediately after handing it to us. */
    t->argv = av_calloc(argc + 1, sizeof(*t->argv));
    if (!t->argv) {
        av_free(t);
        return NULL;
    }
    for (int i = 0; i < argc; i++) {
        t->argv[i] = av_strdup(argv[i] ? argv[i] : "");
        if (!t->argv[i]) {
            for (int j = 0; j < i; j++)
                av_free(t->argv[j]);
            av_free(t->argv);
            av_free(t);
            return NULL;
        }
    }
    t->argv[argc] = NULL;
    t->argc       = argc;
    t->argv_owned = 1;

    atomic_init(&t->cancel_flag, 0);
    atomic_init(&t->error_emitted, 0);
    t->ret_code = 0;

    return t;
}

void ffmpeg_task_set_id(FFmpegTask *t, int id)               { if (t) t->task_id = id; }
void ffmpeg_task_set_user_data(FFmpegTask *t, void *u)       { if (t) t->user_data = u; }
void ffmpeg_task_set_progress_cb(FFmpegTask *t,
                                 ffmpeg_progress_cb cb)      { if (t) t->on_progress = cb; }
void ffmpeg_task_set_error_cb   (FFmpegTask *t,
                                 ffmpeg_error_cb cb)         { if (t) t->on_error    = cb; }
void ffmpeg_task_set_finish_cb  (FFmpegTask *t,
                                 ffmpeg_finish_cb cb)        { if (t) t->on_finish   = cb; }

void ffmpeg_task_cancel(FFmpegTask *t)
{
    if (t)
        atomic_store(&t->cancel_flag, 1);
}

/* ------------------------------------------------------------------------- */
/* Execution                                                                 */
/* ------------------------------------------------------------------------- */

static int run_task_body(FFmpegTask *task)
{
    int ret;

    pthread_once(&g_init_once, engine_global_init);

    /* Bind the task to the current thread so callbacks fired from deep
     * inside the engine can locate it. */
    ffmpeg_task_set_current(task);

    /* Reset all _Thread_local engine state inherited from a previous run
     * on this same thread (or default-init noise). */
    ffmpeg_state_reset();

    /* ffmpeg_run() takes g_options_mutex for the parse_options phase only
     * and runs transcode lock-free (see comment at the top of this file). */
    ret = ffmpeg_run(task->argc, task->argv);

    task->ret_code = ret;

    ffmpeg_task_set_current(NULL);

    if (task->on_finish)
        task->on_finish(task, ret, task->user_data);

    return ret;
}

int ffmpeg_task_run_sync(FFmpegTask *task)
{
    if (!task)
        return AVERROR(EINVAL);
    return run_task_body(task);
}

static void *task_thread_entry(void *arg)
{
    FFmpegTask *task = arg;
    run_task_body(task);
    return NULL;
}

int ffmpeg_task_run_async(FFmpegTask *task)
{
    int err;

    if (!task)
        return AVERROR(EINVAL);
    if (task->thread_started)
        return AVERROR(EBUSY);

    err = pthread_create(&task->thread, NULL, task_thread_entry, task);
    if (err) {
        av_log(NULL, AV_LOG_ERROR,
               "ffmpeg_task_run_async: pthread_create failed: %s\n",
               av_err2str(AVERROR(err)));
        return AVERROR(err);
    }
    task->thread_started = 1;
    return 0;
}

int ffmpeg_task_join(FFmpegTask *task)
{
    if (!task)
        return AVERROR(EINVAL);
    if (!task->thread_started)
        return task->ret_code;
    if (task->thread_joined)
        return task->ret_code;

    pthread_join(task->thread, NULL);
    task->thread_joined = 1;
    return task->ret_code;
}

void ffmpeg_task_free(FFmpegTask **ptask)
{
    FFmpegTask *t;
    if (!ptask || !*ptask)
        return;
    t = *ptask;

    /* Make sure the worker is gone before tearing argv down. */
    if (t->thread_started && !t->thread_joined) {
        atomic_store(&t->cancel_flag, 1);
        pthread_join(t->thread, NULL);
        t->thread_joined = 1;
    }

    if (t->argv_owned && t->argv) {
        for (int i = 0; i < t->argc; i++)
            av_free(t->argv[i]);
        av_free(t->argv);
    }
    av_free(t);
    *ptask = NULL;
}
