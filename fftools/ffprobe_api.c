/*
 * Embeddable ffprobe engine API – per-task error / finish callbacks and
 * worker-thread helpers. Captures the textual ffprobe output via a
 * temporary file and hands it to the caller as an in-memory buffer.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pthread.h>

#include "libavformat/avformat.h"
#include "libavutil/avstring.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#if CONFIG_AVDEVICE
#include "libavdevice/avdevice.h"
#endif

#include "cmdutils.h"
#include "ffprobe_api.h"

/* The log_mutex is owned by ffprobe.c and used by its log_callback(). */
#if HAVE_THREADS
extern pthread_mutex_t log_mutex;
#endif

/* ------------------------------------------------------------------------- */
/* Process-wide one-shot initialization.                                     */
/* ------------------------------------------------------------------------- */

static pthread_once_t g_init_once   = PTHREAD_ONCE_INIT;
static int            g_init_status = 0;

static void engine_global_init(void)
{
    init_dynload();
#if HAVE_THREADS
    if (pthread_mutex_init(&log_mutex, NULL) != 0) {
        g_init_status = AVERROR(EAGAIN);
        return;
    }
#endif
    avformat_network_init();
#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
    g_init_status = 0;
}

/* ------------------------------------------------------------------------- */
/* Concurrency (fine-grained):                                               */
/*   Most of ffprobe's mutable state is now _Thread_local (do_show_*,        */
/*   read_intervals, sections runtime, captured output, ...). The 14         */
/*   process-wide originals that the static real_options[] table binds to   */
/*   are mutated only inside parse_options(), which is serialized through    */
/*   g_options_mutex inside ffprobe_run() itself. Right after parsing those  */
/*   values are snapshot into per-thread copies and the mutex is released,  */
/*   so the probe / writer phase runs lock-free and concurrent ffprobe_run() */
/*   invocations execute in true parallel.                                   */
/*                                                                           */
/*   This file therefore does NOT take any extra mutex.                      */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* FFprobeTask                                                               */
/* ------------------------------------------------------------------------- */

struct FFprobeTask {
    int                 argc;
    char              **argv;       /* deep-copied, owned                */
    int                 argv_owned;

    int                 task_id;
    void               *user_data;

    ffprobe_error_cb    on_error;
    ffprobe_finish_cb   on_finish;

    atomic_int          cancel_flag;
    atomic_int          error_emitted;

    pthread_t           thread;
    int                 thread_started;
    int                 thread_joined;

    /* Captured output. */
    char               *output_text;     /* malloc'd, NUL-terminated   */
    size_t              output_size;
    char               *output_format;   /* "json" / "xml" / ...        */
    int                 ret_code;

    /* Temporary file used to capture ffprobe's stdout-like output. */
    char                tmp_path[64];
    int                 tmp_in_use;       /* 1 if argv contained "-o ..." we appended */
};

static _Thread_local FFprobeTask *tls_current_task = NULL;

FFprobeTask *ffprobe_task_current(void) { return tls_current_task; }
void ffprobe_task_set_current(FFprobeTask *t) { tls_current_task = t; }

int ffprobe_task_is_cancelled(const FFprobeTask *t)
{
    return t ? atomic_load(&((FFprobeTask *)t)->cancel_flag) : 0;
}

void ffprobe_task_emit_error(int errcode, const char *msg)
{
    FFprobeTask *t;
    int expected = 0;

    t = tls_current_task;
    if (!t || !t->on_error)
        return;
    if (atomic_compare_exchange_strong(&t->error_emitted, &expected, 1))
        t->on_error(t, errcode, msg ? msg : "", t->user_data);
}

/* ------------------------------------------------------------------------- */
/* argv helpers                                                              */
/* ------------------------------------------------------------------------- */

static int argv_dup(int argc, char **argv, char ***pout)
{
    char **dst;

    if (argc <= 0 || !argv) {
        *pout = NULL;
        return AVERROR(EINVAL);
    }

    dst = av_calloc(argc + 1, sizeof(*dst));
    if (!dst)
        return AVERROR(ENOMEM);

    for (int i = 0; i < argc; i++) {
        dst[i] = av_strdup(argv[i] ? argv[i] : "");
        if (!dst[i]) {
            for (int j = 0; j < i; j++)
                av_free(dst[j]);
            av_free(dst);
            return AVERROR(ENOMEM);
        }
    }
    dst[argc] = NULL;
    *pout = dst;
    return 0;
}

static void argv_free(int argc, char **argv)
{
    if (!argv) return;
    for (int i = 0; i < argc; i++)
        av_free(argv[i]);
    av_free(argv);
}

/* Search for an option in argv. Returns the index of the FIRST match or -1.
 * The list of canonical names plus accepted aliases is given as a NULL-
 * terminated array. */
static int argv_find_opt(int argc, char **argv, const char *const *names)
{
    int i;
    const char *opt;

    for (i = 1; i < argc; i++) {
        if (!argv[i] || argv[i][0] != '-')
            continue;
        opt = argv[i] + 1;
        for (const char *const *n = names; *n; n++)
            if (!strcmp(opt, *n))
                return i;
    }
    return -1;
}

/* Insert two strings as new argv entries before the trailing NULL.
 * argc grows by 2. argv must have been allocated with one extra slot. */
static int argv_append2(int *pargc, char ***pargv, const char *a, const char *b)
{
    int     argc = *pargc;
    char  **argv = *pargv;
    char  **dst;

    dst = av_calloc(argc + 3, sizeof(*dst));
    if (!dst) return AVERROR(ENOMEM);

    for (int i = 0; i < argc; i++)
        dst[i] = argv[i];

    dst[argc + 0] = av_strdup(a);
    dst[argc + 1] = av_strdup(b);
    dst[argc + 2] = NULL;

    if (!dst[argc + 0] || !dst[argc + 1]) {
        av_free(dst[argc + 0]);
        av_free(dst[argc + 1]);
        av_free(dst);
        return AVERROR(ENOMEM);
    }

    av_free(argv);                /* the old vector itself, NOT its strings */
    *pargv = dst;
    *pargc = argc + 2;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

FFprobeTask *ffprobe_task_alloc(int argc, char **argv)
{
    FFprobeTask *t;
    int          ret;

    if (argc <= 0 || !argv)
        return NULL;

    t = av_mallocz(sizeof(*t));
    if (!t) return NULL;

    ret = argv_dup(argc, argv, &t->argv);
    if (ret < 0) { av_free(t); return NULL; }
    t->argc       = argc;
    t->argv_owned = 1;

    atomic_init(&t->cancel_flag,    0);
    atomic_init(&t->error_emitted,  0);

    /* Default the output format to JSON if the caller did not pick one. */
    {
        static const char *const fmt_names[] =
            { "of", "output_format", "print_format", NULL };
        int idx = argv_find_opt(t->argc, t->argv, fmt_names);
        if (idx < 0) {
            ret = argv_append2(&t->argc, &t->argv, "-of", "json");
            if (ret < 0) { ffprobe_task_free(&t); return NULL; }
            t->output_format = av_strdup("json");
        } else if (idx + 1 < t->argc) {
            t->output_format = av_strdup(t->argv[idx + 1]);
        }
    }

    /* If the user did not supply "-o", create a temporary capture file. */
    {
        static const char *const o_names[] = { "o", NULL };
        int idx = argv_find_opt(t->argc, t->argv, o_names);
        if (idx < 0) {
            int fd;

            snprintf(t->tmp_path, sizeof(t->tmp_path),
                     "/tmp/ffprobe_api_XXXXXX");
            fd = mkstemp(t->tmp_path);
            if (fd < 0) {
                ffprobe_task_free(&t);
                return NULL;
            }
            close(fd);

            ret = argv_append2(&t->argc, &t->argv, "-o", t->tmp_path);
            if (ret < 0) {
                unlink(t->tmp_path);
                ffprobe_task_free(&t);
                return NULL;
            }
            t->tmp_in_use = 1;
        }
    }

    return t;
}

void ffprobe_task_set_id(FFprobeTask *t, int id)            { if (t) t->task_id = id; }
void ffprobe_task_set_user_data(FFprobeTask *t, void *u)    { if (t) t->user_data = u; }
void ffprobe_task_set_error_cb (FFprobeTask *t,
                                ffprobe_error_cb cb)        { if (t) t->on_error  = cb; }
void ffprobe_task_set_finish_cb(FFprobeTask *t,
                                ffprobe_finish_cb cb)       { if (t) t->on_finish = cb; }

void ffprobe_task_cancel(FFprobeTask *t)
{
    if (t) atomic_store(&t->cancel_flag, 1);
}

const char *ffprobe_task_output(const FFprobeTask *t)
{
    return t ? t->output_text : NULL;
}

size_t ffprobe_task_output_size(const FFprobeTask *t)
{
    return t ? t->output_size : 0;
}

int ffprobe_task_id(const FFprobeTask *t)  { return t ? t->task_id  : 0; }
int ffprobe_task_ret(const FFprobeTask *t) { return t ? t->ret_code : -1; }

/* ------------------------------------------------------------------------- */
/* Output capture                                                            */
/* ------------------------------------------------------------------------- */

static int slurp_file(const char *path, char **pdata, size_t *psize)
{
    FILE  *f;
    size_t cap = 0, len = 0;
    char  *buf = NULL;
    char   chunk[4096];

    f = fopen(path, "rb");
    if (!f) return AVERROR(errno);

    for (;;) {
        size_t n = fread(chunk, 1, sizeof(chunk), f);
        if (n == 0) break;
        if (len + n + 1 > cap) {
            size_t ncap = (cap ? cap * 2 : 8192);
            char  *nbuf;
            while (ncap < len + n + 1) ncap *= 2;
            nbuf = av_realloc(buf, ncap);
            if (!nbuf) { av_free(buf); fclose(f); return AVERROR(ENOMEM); }
            buf = nbuf;
            cap = ncap;
        }
        memcpy(buf + len, chunk, n);
        len += n;
    }

    fclose(f);

    if (!buf) {
        buf = av_mallocz(1);
        if (!buf) return AVERROR(ENOMEM);
    } else {
        buf[len] = '\0';
    }

    *pdata = buf;
    *psize = len;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Execution                                                                 */
/* ------------------------------------------------------------------------- */

static int run_task_body(FFprobeTask *task)
{
    int ret;

    pthread_once(&g_init_once, engine_global_init);
    if (g_init_status != 0)
        return g_init_status;

    /* No global engine lock: ffprobe_run() takes g_options_mutex itself
     * for the parse_options phase only and runs probe lock-free. */
    ffprobe_task_set_current(task);
    ffprobe_state_reset();

    ret = ffprobe_run(task->argc, task->argv);
    task->ret_code = ret;

    /* Each task uses a unique tmp_path (mkstemp) so this is safe to read
     * concurrently with other workers. */
    if (task->tmp_in_use) {
        char  *data = NULL;
        size_t size = 0;
        int    rret = slurp_file(task->tmp_path, &data, &size);
        if (rret >= 0) {
            task->output_text = data;
            task->output_size = size;
        }
        unlink(task->tmp_path);
    }

    ffprobe_task_set_current(NULL);

    if (task->on_finish) {
        FFprobeResult r = {
            .text   = task->output_text,
            .size   = task->output_size,
            .ret    = ret,
            .format = task->output_format,
        };
        task->on_finish(task, &r, task->user_data);
    }

    return ret;
}

int ffprobe_task_run_sync(FFprobeTask *task)
{
    if (!task) return AVERROR(EINVAL);
    return run_task_body(task);
}

static void *task_thread_entry(void *arg)
{
    run_task_body(arg);
    return NULL;
}

int ffprobe_task_run_async(FFprobeTask *task)
{
    int err;

    if (!task)                  return AVERROR(EINVAL);
    if (task->thread_started)   return AVERROR(EBUSY);

    err = pthread_create(&task->thread, NULL, task_thread_entry, task);
    if (err) {
        av_log(NULL, AV_LOG_ERROR,
               "ffprobe_task_run_async: pthread_create failed: %s\n",
               av_err2str(AVERROR(err)));
        return AVERROR(err);
    }
    task->thread_started = 1;
    return 0;
}

int ffprobe_task_join(FFprobeTask *task)
{
    if (!task)                                     return AVERROR(EINVAL);
    if (!task->thread_started)                     return task->ret_code;
    if (task->thread_joined)                       return task->ret_code;

    pthread_join(task->thread, NULL);
    task->thread_joined = 1;
    return task->ret_code;
}

void ffprobe_task_free(FFprobeTask **ptask)
{
    FFprobeTask *t;

    if (!ptask || !*ptask) return;
    t = *ptask;

    if (t->thread_started && !t->thread_joined) {
        atomic_store(&t->cancel_flag, 1);
        pthread_join(t->thread, NULL);
        t->thread_joined = 1;
    }

    if (t->tmp_in_use && t->tmp_path[0])
        unlink(t->tmp_path); /* harmless if already gone */

    if (t->argv_owned)
        argv_free(t->argc, t->argv);

    av_free(t->output_text);
    av_free(t->output_format);
    av_free(t);
    *ptask = NULL;
}
