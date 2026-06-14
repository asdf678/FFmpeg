/*
 * Demo: drive two concurrent ffmpeg transcode tasks from the main thread,
 *       each in its own worker thread, with progress / error / finish
 *       callbacks and per-task TLS isolation.
 *
 * This file is *not* part of the default ffmpeg build (it defines its own
 * main(), which would collide with fftools/ffmpeg.c). To build the demo,
 * link the standard fftools objects but exclude ffmpeg.o's main(). Suggested
 * out-of-tree build flow:
 *
 *   1) Build the regular ffmpeg first so all fftools/*.o are produced:
 *        make -C build -j$(nproc)
 *
 *   2) Compile a "driver" version of ffmpeg.o that hides its main() :
 *        gcc -Iinstall/include -DFFMPEG_DRIVER_NO_MAIN -c \
 *            fftools/ffmpeg.c -o build/fftools/ffmpeg_nomain.o
 *      (This requires wrapping main() with the macro – see the patch hint
 *      at the bottom of this file.)
 *
 *   3) Link the demo against fftools objects + FFmpeg shared libs:
 *        gcc fftools/ffmpeg_api_demo.c \
 *            build/fftools/ffmpeg_api.o \
 *            build/fftools/ffmpeg_nomain.o \
 *            build/fftools/ffmpeg_dec.o build/fftools/ffmpeg_demux.o \
 *            build/fftools/ffmpeg_enc.o build/fftools/ffmpeg_filter.o \
 *            build/fftools/ffmpeg_hw.o  build/fftools/ffmpeg_mux.o \
 *            build/fftools/ffmpeg_mux_init.o build/fftools/ffmpeg_opt.o \
 *            build/fftools/ffmpeg_sched.o   build/fftools/objpool.o \
 *            build/fftools/sync_queue.o     build/fftools/thread_queue.o \
 *            build/fftools/cmdutils.o       build/fftools/opt_common.o \
 *            -L install/lib -lavformat -lavcodec -lavfilter -lavdevice \
 *            -lswresample -lswscale -lavutil -lpthread -lm \
 *            -o build/ffmpeg_api_demo
 *
 *   4) Run two concurrent transcodes:
 *        LD_LIBRARY_PATH=install/lib ./build/ffmpeg_api_demo \
 *            in1.mp4 out1.mkv  in2.mp4 out2.mkv
 *
 * The demo demonstrates:
 *   - main thread allocates two FFmpegTask objects.
 *   - main thread starts each as a worker via pthread_create wrapper.
 *   - each task fires its own progress/error/finish callbacks (1Hz).
 *   - main thread waits with ffmpeg_task_join().
 *   - global state is _Thread_local, so the two tasks do not collide.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fftools/ffmpeg_api.h"

static void on_progress(FFmpegTask *task, const FFmpegProgress *p, void *ud)
{
    (void)task; (void)ud;
    fprintf(stderr,
            "[task %d] t=%6.2fs pts=%6.2fs frame=%-6llu fps=%5.1f "
            "speed=%5.2fx bitrate=%7.1fk size=%lld stage=%d\n",
            p->task_id,
            p->elapsed_us / 1e6,
            p->pts_us     / 1e6,
            (unsigned long long)p->frame_number,
            p->fps,
            p->speed,
            p->bitrate_kbps,
            (long long)p->total_size,
            p->stage);
}

static void on_error(FFmpegTask *task, int code, const char *msg, void *ud)
{
    (void)task; (void)ud;
    fprintf(stderr, "[task %d] ERROR %d: %s\n",
            (int)(intptr_t)ud, code, msg);
}

static void on_finish(FFmpegTask *task, int ret, void *ud)
{
    (void)task;
    fprintf(stderr, "[task %d] FINISHED ret=%d\n", (int)(intptr_t)ud, ret);
}

static FFmpegTask *make_task(int id, const char *in, const char *out)
{
    /* Build argv[]: ffmpeg -re -i <in> -c:v mpeg4 -y <out>
     * '-re' makes the demuxer read at native frame rate, so we get a few
     * progress ticks even on a small input – useful for demo purposes. */
    char *argv[] = {
        "ffmpeg",
        "-hide_banner",
        "-re",
        "-i", (char *)in,
        "-c:v", "mpeg4",
        "-y", (char *)out,
        NULL,
    };
    int argc = 9;

    FFmpegTask *t = ffmpeg_task_alloc(argc, argv);
    if (!t) return NULL;

    ffmpeg_task_set_id(t, id);
    ffmpeg_task_set_user_data(t, (void *)(intptr_t)id);
    ffmpeg_task_set_progress_cb(t, on_progress);
    ffmpeg_task_set_error_cb   (t, on_error);
    ffmpeg_task_set_finish_cb  (t, on_finish);
    return t;
}

int main(int argc, char **argv)
{
    if (argc < 5 || (argc - 1) % 2 != 0) {
        fprintf(stderr,
                "Usage: %s in1 out1 [in2 out2 ...]\n", argv[0]);
        return 1;
    }

    int n = (argc - 1) / 2;
    FFmpegTask **tasks = calloc(n, sizeof(*tasks));
    if (!tasks) return 2;

    /* Spawn one worker thread per (in,out) pair. */
    for (int i = 0; i < n; i++) {
        tasks[i] = make_task(i + 1, argv[1 + 2 * i], argv[2 + 2 * i]);
        if (!tasks[i]) { fprintf(stderr, "alloc failed\n"); return 3; }

        int err = ffmpeg_task_run_async(tasks[i]);
        if (err < 0) {
            fprintf(stderr, "task %d failed to start: %d\n", i + 1, err);
            return 4;
        }
    }

    /* Main thread waits for every worker. */
    int worst = 0;
    for (int i = 0; i < n; i++) {
        int rc = ffmpeg_task_join(tasks[i]);
        if (rc) worst = rc;
        ffmpeg_task_free(&tasks[i]);
    }

    free(tasks);
    return worst;
}

/*
 * --- Patch hint for ffmpeg.c to support driver linking ---
 *
 * Replace the existing main() in fftools/ffmpeg.c with:
 *
 *   #ifndef FFMPEG_DRIVER_NO_MAIN
 *   int main(int argc, char **argv)
 *   {
 *       init_dynload();
 *       setvbuf(stderr, NULL, _IONBF, 0);
 *   #if CONFIG_AVDEVICE
 *       avdevice_register_all();
 *   #endif
 *       avformat_network_init();
 *       return ffmpeg_run(argc, argv);
 *   }
 *   #endif
 *
 * Then compile that file once with -DFFMPEG_DRIVER_NO_MAIN to embed the
 * engine into the demo without symbol clashes.
 */
