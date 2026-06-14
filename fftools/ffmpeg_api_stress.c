/*
 * Stress demo: drive N concurrent CPU-bound transcodes from one process
 * via the embedding API and measure parallel scaling.
 *
 * Each task runs:
 *   ffmpeg -threads 1 -i <in> -vf hqdn3d=8 -c:v mpeg4 -q:v 2 -threads 1 -y <out>
 *
 * `-threads 1` isolates the per-task CPU footprint to ~1 core, so:
 *   wall_seconds(N tasks) / wall_seconds(1 task) ≈ 1   if perfectly parallel
 *                                                  ≈ N   if fully serialized
 *
 * Build the same way as ffmpeg_api_demo.c.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fftools/ffmpeg_api.h"

static double wall_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void on_finish(FFmpegTask *task, int ret, void *ud)
{
    (void)task;
    fprintf(stderr, "[task %d] finished ret=%d\n", (int)(intptr_t)ud, ret);
}

static FFmpegTask *make_task(int id, const char *in, const char *out)
{
    char *argv[] = {
        "ffmpeg", "-hide_banner", "-nostats",
        "-threads", "1",
        "-i", (char *)in,
        "-vf", "hqdn3d=8",
        "-c:v", "mpeg4",
        "-q:v", "2",
        "-threads", "1",
        "-y", (char *)out,
        NULL,
    };
    int argc = 17;

    FFmpegTask *t = ffmpeg_task_alloc(argc, argv);
    if (!t) return NULL;

    ffmpeg_task_set_id(t, id);
    ffmpeg_task_set_user_data(t, (void *)(intptr_t)id);
    ffmpeg_task_set_finish_cb(t, on_finish);
    return t;
}

int main(int argc, char **argv)
{
    if (argc < 3 || (argc - 1) % 2 != 0) {
        fprintf(stderr, "Usage: %s in1 out1 [in2 out2 ...]\n", argv[0]);
        return 1;
    }

    int n = (argc - 1) / 2;
    FFmpegTask **tasks = calloc(n, sizeof(*tasks));
    if (!tasks) return 2;

    fprintf(stderr, "=== running %d concurrent ffmpeg_run() pipelines ===\n", n);
    double t0 = wall_now();

    for (int i = 0; i < n; i++) {
        tasks[i] = make_task(i + 1, argv[1 + 2 * i], argv[2 + 2 * i]);
        if (!tasks[i]) { fprintf(stderr, "alloc failed\n"); return 3; }
        if (ffmpeg_task_run_async(tasks[i]) < 0) {
            fprintf(stderr, "task %d start failed\n", i + 1);
            return 4;
        }
    }

    int worst = 0;
    for (int i = 0; i < n; i++) {
        int rc = ffmpeg_task_join(tasks[i]);
        if (rc) worst = rc;
        ffmpeg_task_free(&tasks[i]);
    }
    free(tasks);

    double dt = wall_now() - t0;
    fprintf(stderr, "=== %d task(s) finished in %.2fs (worst rc=%d) ===\n",
            n, dt, worst);
    return worst;
}
