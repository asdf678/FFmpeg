/*
 * Public C API for embedding ffprobe inside a host program.
 *
 * Provides:
 *   - Per-task error / finish callbacks.
 *   - Synchronous and asynchronous (worker thread) execution.
 *   - The full ffprobe textual output captured into a malloc'd buffer that
 *     is delivered to the finish callback.
 *
 * Multi-thread isolation (fine-grained):
 *   The bulk of ffprobe's mutable state is _Thread_local — every Class-(B)
 *   global (do_show_*, read_intervals, ...), the input/output filenames,
 *   the section runtime table, and the captured output context.
 *   The 14 Class-(A) globals that the static real_options[] table binds
 *   to by &var stay process-wide; they are mutated only inside parse_options(),
 *   which is serialized via g_options_mutex. Right after parsing completes
 *   their values are copied into per-task TLS snapshots and consumed lock-free
 *   by the rest of the pipeline (probe / write).
 *   Net effect: N concurrent ffprobe_run() calls execute N parse phases
 *   serially (~ms each) and then N probe phases truly in parallel. There
 *   is no engine-wide mutex.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef FFTOOLS_FFPROBE_API_H
#define FFTOOLS_FFPROBE_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FFprobeTask FFprobeTask;

/*
 * Result delivered to the finish callback.
 *  - text  : NUL-terminated UTF-8 string with the full ffprobe output;
 *            owned by the FFprobeTask, valid until ffprobe_task_free().
 *            May be NULL if probing failed before any output was produced.
 *  - size  : strlen(text), 0 if text == NULL.
 *  - ret   : the integer return code (0 on success, < 0 on failure).
 *  - format: requested output format, e.g. "default", "json", "xml" ...
 */
typedef struct FFprobeResult {
    const char *text;
    size_t      size;
    int         ret;
    const char *format;
} FFprobeResult;

/* Error callback (invoked on the worker thread, exactly once). */
typedef void (*ffprobe_error_cb)(FFprobeTask *task,
                                 int errcode,
                                 const char *msg,
                                 void *user_data);

/* Finish callback (invoked once per task, regardless of success). */
typedef void (*ffprobe_finish_cb)(FFprobeTask *task,
                                  const FFprobeResult *result,
                                  void *user_data);

/*
 * Allocate a probe task.
 *
 *  argc/argv : an ffprobe-style command line. The strings are deep-copied
 *              and may be released by the caller right after this call.
 *              Note: argv[0] is the program name and is otherwise ignored.
 *
 * If the command line does not include "-output_format / -of" the API
 * defaults to "json" so the caller gets machine-readable output.
 * If "-o <file>" is *not* in the command line the output is captured into
 * an in-memory buffer and delivered to the finish callback. If "-o" is
 * present the output is written to the specified file as usual and the
 * captured buffer will be empty.
 */
FFprobeTask *ffprobe_task_alloc(int argc, char **argv);

void ffprobe_task_set_id        (FFprobeTask *task, int task_id);
void ffprobe_task_set_user_data (FFprobeTask *task, void *user_data);
void ffprobe_task_set_error_cb  (FFprobeTask *task, ffprobe_error_cb cb);
void ffprobe_task_set_finish_cb (FFprobeTask *task, ffprobe_finish_cb cb);

/* Run on the calling thread. Blocks until the task completes. */
int  ffprobe_task_run_sync (FFprobeTask *task);

/* Run on a private worker thread (pthread_create wrapper). */
int  ffprobe_task_run_async(FFprobeTask *task);

/* Request cooperative cancellation. Safe from any thread. */
void ffprobe_task_cancel(FFprobeTask *task);

/* Wait for an async task to terminate; returns its return code. */
int  ffprobe_task_join(FFprobeTask *task);

/*
 * Direct accessors for the most common field – usable without a finish
 * callback when you call ffprobe_task_run_sync() / ffprobe_task_join().
 * The pointers remain valid until ffprobe_task_free().
 */
const char *ffprobe_task_output (const FFprobeTask *task);
size_t      ffprobe_task_output_size(const FFprobeTask *task);
int         ffprobe_task_id     (const FFprobeTask *task);
int         ffprobe_task_ret    (const FFprobeTask *task);

/* Free task resources. Must be called after join() / sync run. */
void ffprobe_task_free(FFprobeTask **ptask);

/* Internal helpers used by ffprobe.c – not for application code. */
FFprobeTask *ffprobe_task_current(void);
void         ffprobe_task_set_current(FFprobeTask *task);
int          ffprobe_task_is_cancelled(const FFprobeTask *task);
void         ffprobe_task_emit_error(int errcode, const char *msg);

/* Engine entry points implemented in ffprobe.c. */
int  ffprobe_run                  (int argc, char **argv);
void ffprobe_state_reset          (void);
void ffprobe_options_lock         (void);
void ffprobe_options_unlock       (void);
void ffprobe_snapshot_opts_to_tls (void);

#ifdef __cplusplus
}
#endif

#endif /* FFTOOLS_FFPROBE_API_H */
