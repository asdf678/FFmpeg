/*
 * Demo: drive several ffprobe tasks concurrently from the main thread
 * with finish / error callbacks and per-task isolation.
 *
 * NOT part of the default build (defines its own main()). Build the regular
 * binaries first, then:
 *
 *   gcc -DFFPROBE_DRIVER_NO_MAIN -I. -I.. -I./fftools -I../fftools \
 *       -c ../fftools/ffprobe.c -o fftools/ffprobe_nomain.o
 *
 *   gcc -I. -I.. -I./fftools -I../fftools \
 *       ../fftools/ffprobe_api_demo.c \
 *       fftools/ffprobe_api.o   fftools/ffprobe_nomain.o \
 *       fftools/cmdutils.o      fftools/opt_common.o \
 *       -L libavformat -lavformat -L libavcodec -lavcodec \
 *       -L libavfilter -lavfilter -L libavdevice -lavdevice \
 *       -L libavutil -lavutil   -L libswresample -lswresample \
 *       -L libswscale -lswscale -L libpostproc -lpostproc \
 *       -lpthread -lm -o ffprobe_api_demo
 *
 *   LD_LIBRARY_PATH=...:build/libavformat:... \
 *   ./build/ffprobe_api_demo file1.mp4 file2.mp4 ...
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fftools/ffprobe_api.h"

static void on_error(FFprobeTask *task, int code, const char *msg, void *ud)
{
    fprintf(stderr, "[task %d] ERROR %d: %s\n",
            (int)(intptr_t)ud, code, msg);
}

static void on_finish(FFprobeTask *task, const FFprobeResult *r, void *ud)
{
    int id = (int)(intptr_t)ud;
    fprintf(stderr,
            "[task %d] FINISHED ret=%d format=%s size=%zu bytes\n",
            id, r->ret, r->format ? r->format : "(none)", r->size);

    if (r->text && r->size > 0) {
        size_t shown = r->size < 240 ? r->size : 240;
        fprintf(stderr, "[task %d] preview: %.*s%s\n",
                id, (int)shown, r->text,
                r->size > shown ? " ..." : "");
    }
}

static FFprobeTask *make_task(int id, const char *file)
{
    char *argv[] = {
        "ffprobe",                /* [0] */
        "-hide_banner",           /* [1] */
        "-loglevel", "error",     /* [2..3] */
        "-count_frames",          /* [4]   force decode-all */
        "-count_packets",         /* [5]   force demux-all */
        "-show_streams",          /* [6] */
        (char *)file,             /* [7] */
        NULL,
    };
    int argc = 8;

    FFprobeTask *t = ffprobe_task_alloc(argc, argv);
    if (!t) return NULL;

    ffprobe_task_set_id(t, id);
    ffprobe_task_set_user_data(t, (void *)(intptr_t)id);
    ffprobe_task_set_error_cb (t, on_error);
    ffprobe_task_set_finish_cb(t, on_finish);
    return t;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s file1 [file2 ...]\n", argv[0]);
        return 1;
    }

    int n = argc - 1;
    FFprobeTask **tasks = calloc(n, sizeof(*tasks));
    if (!tasks) return 2;

    /* Main thread fans out one worker thread per file. */
    for (int i = 0; i < n; i++) {
        tasks[i] = make_task(i + 1, argv[i + 1]);
        if (!tasks[i]) { fprintf(stderr, "alloc failed\n"); return 3; }
        int err = ffprobe_task_run_async(tasks[i]);
        if (err < 0) {
            fprintf(stderr, "task %d failed to start: %d\n", i + 1, err);
            return 4;
        }
    }

    /* Main thread waits for every worker. */
    int worst = 0;
    for (int i = 0; i < n; i++) {
        int rc = ffprobe_task_join(tasks[i]);
        if (rc) worst = rc;

        /* Output is also reachable directly without going through the cb. */
        const char *txt = ffprobe_task_output(tasks[i]);
        size_t      sz  = ffprobe_task_output_size(tasks[i]);
        fprintf(stderr, "[task %d] direct accessor: %zu bytes captured\n",
                ffprobe_task_id(tasks[i]), sz);
        (void)txt;

        ffprobe_task_free(&tasks[i]);
    }

    free(tasks);
    return worst < 0 ? 5 : 0;
}
