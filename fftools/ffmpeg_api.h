/*
 * Public C API for embedding the ffmpeg CLI engine inside a host program.
 *
 * Provides:
 *   - Per-task progress / error / finish callbacks.
 *   - Synchronous and asynchronous (worker thread) execution.
 *   - Per-task cancel flag.
 *
 * Multi-thread isolation:
 *   The traditional global state used by the ffmpeg CLI (input/output file
 *   tables, filtergraphs, option variables, vstats handles, progress_avio,
 *   etc.) is tagged _Thread_local in this build. Each worker thread therefore
 *   sees its own private copy and tasks executed concurrently on different
 *   threads do not stomp on each other.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef FFTOOLS_FFMPEG_API_H
#define FFTOOLS_FFMPEG_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stage carried by the progress callback. */
typedef enum FFmpegProgressStage {
    FFMPEG_STAGE_INIT     = 0, /* before transcode loop                */
    FFMPEG_STAGE_RUNNING  = 1, /* inside transcode loop (1Hz heartbeat)*/
    FFMPEG_STAGE_FINAL    = 2, /* last report                          */
} FFmpegProgressStage;

/* Snapshot of one progress tick. */
typedef struct FFmpegProgress {
    int      task_id;          /* user-supplied task id              */
    int      stage;             /* FFmpegProgressStage              */

    int64_t  elapsed_us;        /* wall-clock time since task start */
    int64_t  pts_us;            /* current output PTS, AV_TIME_BASE */
    int64_t  total_size;        /* output bytes written, -1 if N/A  */

    uint64_t frame_number;      /* video frames written              */
    double   fps;
    double   bitrate_kbps;      /* -1 if unknown                    */
    double   speed;             /* -1 if unknown                    */
    float    quality;
    uint64_t dup_frames;
    uint64_t drop_frames;
} FFmpegProgress;

typedef struct FFmpegTask FFmpegTask;

/* 1Hz progress callback (invoked from the worker thread). */
typedef void (*ffmpeg_progress_cb)(FFmpegTask *task,
                                   const FFmpegProgress *p,
                                   void *user_data);

/* Error callback (invoked once when the task aborts abnormally). */
typedef void (*ffmpeg_error_cb)(FFmpegTask *task,
                                int errcode,
                                const char *msg,
                                void *user_data);

/* Final completion callback. ret_code is 0 on success, < 0 on failure,
 * 255 on user/cancel signal. */
typedef void (*ffmpeg_finish_cb)(FFmpegTask *task,
                                 int ret_code,
                                 void *user_data);

/* Allocate a task. argv must remain valid until the task finishes. */
FFmpegTask *ffmpeg_task_alloc(int argc, char **argv);

/* Configure the task. */
void ffmpeg_task_set_id(FFmpegTask *task, int task_id);
void ffmpeg_task_set_user_data(FFmpegTask *task, void *user_data);
void ffmpeg_task_set_progress_cb(FFmpegTask *task, ffmpeg_progress_cb cb);
void ffmpeg_task_set_error_cb   (FFmpegTask *task, ffmpeg_error_cb    cb);
void ffmpeg_task_set_finish_cb  (FFmpegTask *task, ffmpeg_finish_cb   cb);

/* Run the task on the calling thread. Blocks until it finishes. */
int  ffmpeg_task_run_sync (FFmpegTask *task);

/* Spawn a worker thread for the task. ffmpeg_task_join() is required. */
int  ffmpeg_task_run_async(FFmpegTask *task);

/* Request cooperative cancellation. Safe to call from any thread. */
void ffmpeg_task_cancel(FFmpegTask *task);

/* Wait for an async task to terminate. Returns the task's exit code. */
int  ffmpeg_task_join(FFmpegTask *task);

/* Free task resources. Must be called after join() / sync run. */
void ffmpeg_task_free(FFmpegTask **ptask);

/* Internal helpers used by ffmpeg.c – not for application code. */
struct FFmpegTask *ffmpeg_task_current(void);
void               ffmpeg_task_set_current(FFmpegTask *task);
int                ffmpeg_task_is_cancelled(const FFmpegTask *task);
void               ffmpeg_task_emit_progress(const FFmpegProgress *p);
void               ffmpeg_task_emit_error(int errcode, const char *msg);

#ifdef __cplusplus
}
#endif

#endif /* FFTOOLS_FFMPEG_API_H */
