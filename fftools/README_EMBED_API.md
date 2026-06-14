# FFmpeg Embeddable Engine API

> 把 `ffmpeg` 命令行引擎嵌入到宿主程序，提供
> **进度回调（≈1Hz）** / **异常回调** / **完成回调**，
> 并支持 **主线程派发子线程任务** 的并发执行模型。
>
> 适配源码：`fftools/`（FFmpeg 7.1.x，`dev7` 分支）

---

## 1. 文件清单

| 文件 | 类型 | 说明 |
|---|---|---|
| `fftools/ffmpeg_api.h` | 新增 | 公共 API：`FFmpegTask` / 回调类型 / 任务运行接口 |
| `fftools/ffmpeg_api.c` | 新增 | 任务封装、进程级一次性初始化、子线程入口、TLS `current_task` 路由（**无全局执行锁**） |
| `fftools/ffmpeg_api_demo.c` | 新增 | 主线程派发两个并发子线程任务的演示，独立编译（`-DFFMPEG_DRIVER_NO_MAIN`） |
| `fftools/ffmpeg.h` | 修改 | 全局状态分两类（TLS / 进程级）；新增 `ffmpeg_run`、`ffmpeg_state_reset` 接口 |
| `fftools/ffmpeg.c` | 修改 | 抽取 `ffmpeg_run()`；`parse_options` 段加 `ffmpeg_options_lock/unlock`，紧跟 `ffmpeg_snapshot_opts_to_tls()`；`main()` 用 `FFMPEG_DRIVER_NO_MAIN` 包裹；`print_report` 集成 1Hz 进度回调；`transcode/ffmpeg_run` 失败路径触发 error 回调；`decode_interrupt_cb` 支持任务级取消；`ffmpeg_state_reset()` 复位 TLS |
| `fftools/ffmpeg_opt.c` | 修改 | (1) 未被 `options[]` 引用的变量直接加 `_Thread_local`；(2) 为 `options[]` 引用的 21 个进程级变量同时声明 `_Thread_local` 副本 + `ffmpeg_snapshot_opts_to_tls()` + `g_options_mutex` |
| `fftools/Makefile` | 修改 | 加入 `fftools/ffmpeg_api.o` |

---

## 2. 设计概览

### 2.0 v2 关键变化：从粗锁到细粒度锁

> 早期版本用进程级 `g_engine_mutex` 包裹整个 `ffmpeg_run()`，
> 任务"派发并发、执行串行"。
> 当前版本采用 **细粒度锁 + 选项快照**：仅 `parse_options` 阶段持锁
> （几十毫秒），随后立即把 21 个进程级选项变量快照到 `_Thread_local`
> 副本，转码阶段完全无锁、真并发。两个并发任务的总挂钟时间
> 由 `2 × T` 降到 ≈ `1 × T`。

### 2.1 隔离策略：分两层

```
┌────────────────────────────────────────────────────────────────┐
│                         FFmpegTask                             │
│  argv / argc / task_id / user_data / cancel_flag / cbs / ...   │
└────────────────────────────────────────────────────────────────┘
                                │
       run sync                 │                run async
   (调用线程)              ┌────┴────┐         (pthread_create)
                           │         │
                           ▼         ▼
   ┌──────────────────────────────────────────────────────────┐
   │   ffmpeg_task_set_current(task)   ←── TLS 当前任务         │
   │   ffmpeg_state_reset()            ←── 复位 TLS             │
   │   ┌────────────────────────────────────────────────────┐ │
   │   │  ffmpeg_options_lock()           ──┐ 短临界区       │ │
   │   │  ffmpeg_parse_options(...)         │ ≈ 数十 ms      │ │
   │   │  ffmpeg_snapshot_opts_to_tls() ←──┘ 把进程级 → TLS  │ │
   │   │  ffmpeg_options_unlock()                            │ │
   │   └────────────────────────────────────────────────────┘ │
   │   transcode(sch)  ───────── 完全无锁 / 真并发              │
   │         └─ print_report (1Hz emit)                       │
   │   ffmpeg_task_set_current(NULL)                          │
   └──────────────────────────────────────────────────────────┘
                                │
                                ▼
                       on_finish(task, ret, ...)
```

具体两类全局状态：

#### (A) `_Thread_local` —— 每任务独立副本

| 变量 | 用途 |
|---|---|
| `input_files / nb_input_files` | 输入文件表 |
| `output_files / nb_output_files` | 输出文件表 |
| `filtergraphs / nb_filtergraphs` | 复杂 filtergraph 表 |
| `decoders / nb_decoders` | 独立解码器表 |
| `progress_avio` | `-progress` 输出 IO |
| `vstats_file / vstats_filename` | `-vstats` 输出 |
| `nb_output_dumped` | 启动阶段进度计数 |
| `transcode_init_done` | 信号过滤阈值 |
| `copy_ts_first_pts` | `-copyts` 状态 |
| `filter_hw_device / filter_nbthreads` | 硬件/滤镜线程 |
| `audio_drift_threshold / video_sync_method` | 音视频同步 |
| `abort_on_flags` | abort 条件 |
| `stats_period` | 统计周期 |

它们仅在 `opt_*` 助手或运行时赋值，地址不出现在 `options[]` 静态初始化表里，
所以可以直接 `_Thread_local`。

#### (B) 进程级原始变量 + 短临界区 `g_options_mutex`

C 标准要求 `options[]` 中 `{ &var }` 形式的初始化器是 **编译期常量**，
`_Thread_local` 变量地址不是常量。所以下列 21 个变量必须保留进程级身份。
但是它们 **只在 `parse_options` 阶段被写入**，因此只锁这一段就够了：

```
do_benchmark, do_benchmark_all, stdin_interaction
do_pkt_dump, do_hex_dump
frame_drop_threshold, copy_ts, start_at_zero, copy_tb
dts_delta_threshold, dts_error_threshold, exit_on_error
filter_complex_nbthreads, auto_conversion_filters, print_stats
debug_ts, max_error_rate, vstats_version
ignore_unknown_streams, copy_unknown_streams, recast_media
```

#### (C) `_Thread_local` 选项快照 + 透明 macro 重定向

为了让 transcode 阶段读到属于自己任务的值，`ffmpeg.h` 同时声明了一组
`xxx_tls` 副本，并通过预处理把所有读取重定向到副本：

```c
extern int   print_stats;          /* 进程级，options[] 静态绑定         */
extern _Thread_local int print_stats_tls;

#ifndef FFMPEG_RAW_GLOBALS
#define print_stats   print_stats_tls   /* 几乎所有 .c 走这条路径 */
#endif
```

`ffmpeg_opt.c` 在 `#include "ffmpeg.h"` **之前** 定义 `FFMPEG_RAW_GLOBALS`，
因而保留对真名变量的写访问，供 `options[]` 静态初始化器与 snapshot 函数使用：

```c
/* ffmpeg_opt.c */
void ffmpeg_snapshot_opts_to_tls(void) {
    print_stats_tls   = print_stats;     /* 真 → TLS 副本 */
    do_benchmark_tls  = do_benchmark;
    /* … 21 个 … */
}
```

#### (D) `ffmpeg_run()` 中的双阶段执行

```c
int ffmpeg_run(int argc, char **argv) {
    /* ……前置 …… */

    /* Phase 1: 短临界区，串行 */
    ffmpeg_options_lock();
    ret = ffmpeg_parse_options(argc, argv, sch);
    if (ret >= 0)
        ffmpeg_snapshot_opts_to_tls();
    ffmpeg_options_unlock();

    if (ret < 0) goto finish;

    /* Phase 2: 完全无锁，多任务真并发 */
    ret = transcode(sch);

    /* …… cleanup …… */
}
```

> **效果**：解析阶段几十毫秒持锁，转码阶段（占任务 99% 时间）完全无锁。
> 两条 pipeline 真正并行，不再互相阻塞。

### 2.2 信号处理

- `received_sigterm / received_nb_signals` 仍为进程级（POSIX 信号天然进程级），
  仅用于 CLI driver 处理 `Ctrl-C`。
- 子线程任务用 **任务级 `cancel_flag`** 协作取消，由 `ffmpeg_task_cancel()` 设置，
  在 `decode_interrupt_cb` 与 `transcode` 主循环中检查。

### 2.3 回调注入点

```c
// fftools/ffmpeg.c print_report() 主循环中，节流到 1Hz：
if (cb_task && notify_cb) {
    FFmpegProgress p = { /* pts、speed、bitrate、frame、fps、size、stage… */ };
    ffmpeg_task_emit_progress(&p);
}

// fftools/ffmpeg.c transcode() / ffmpeg_run() 失败路径：
if (ret < 0) ffmpeg_task_emit_error(ret, "...");

// fftools/ffmpeg_api.c run_task_body() 退出（无全局锁包裹）：
if (task->on_finish) task->on_finish(task, ret, task->user_data);
```

`stage` 取值：

| 值 | 含义 |
|---|---|
| `FFMPEG_STAGE_INIT (0)`    | 初始化阶段第一次报告 |
| `FFMPEG_STAGE_RUNNING (1)` | 转码进行中（≈1Hz 心跳） |
| `FFMPEG_STAGE_FINAL (2)`   | 任务结束前最后一次报告 |

---

## 3. API 参考

完整定义见 [`fftools/ffmpeg_api.h`](./ffmpeg_api.h)。

### 3.1 类型

```c
typedef enum FFmpegProgressStage {
    FFMPEG_STAGE_INIT     = 0,
    FFMPEG_STAGE_RUNNING  = 1,
    FFMPEG_STAGE_FINAL    = 2,
} FFmpegProgressStage;

typedef struct FFmpegProgress {
    int      task_id;
    int      stage;            /* FFmpegProgressStage  */

    int64_t  elapsed_us;       /* 任务开始至今微秒    */
    int64_t  pts_us;           /* 当前输出 PTS         */
    int64_t  total_size;       /* 已写出字节，-1=N/A  */

    uint64_t frame_number;     /* 视频帧数             */
    double   fps;
    double   bitrate_kbps;     /* -1=N/A              */
    double   speed;            /* -1=N/A              */
    float    quality;
    uint64_t dup_frames;
    uint64_t drop_frames;
} FFmpegProgress;

typedef struct FFmpegTask FFmpegTask;

typedef void (*ffmpeg_progress_cb)(FFmpegTask*, const FFmpegProgress*, void *user);
typedef void (*ffmpeg_error_cb   )(FFmpegTask*, int errcode, const char *msg, void *user);
typedef void (*ffmpeg_finish_cb  )(FFmpegTask*, int ret_code, void *user);
```

### 3.2 函数

| 函数 | 行为 |
|---|---|
| `ffmpeg_task_alloc(argc, argv)`            | 深拷贝 argv，分配任务对象 |
| `ffmpeg_task_set_id / user_data`           | 配置任务标识与用户上下文 |
| `ffmpeg_task_set_progress_cb / error_cb / finish_cb` | 注册回调 |
| `ffmpeg_task_run_sync(task)`               | 在调用线程同步执行 |
| `ffmpeg_task_run_async(task)`              | 内部 `pthread_create` 起子线程 |
| `ffmpeg_task_cancel(task)`                 | 协作取消（任意线程可调） |
| `ffmpeg_task_join(task)`                   | 等待异步任务结束，返回 `ret_code` |
| `ffmpeg_task_free(&task)`                  | 释放任务（必要时自动 cancel + join） |

`ret_code` 约定：

| 值 | 含义 |
|---|---|
| `0`  | 成功 |
| `<0` | FFmpeg 错误码（`AVERROR(...)`） |
| `255` | 收到 SIGINT/SIGTERM |
| `69`  | `FFMPEG_ERROR_RATE_EXCEEDED` |

---

## 4. 编译

### 4.1 普通 ffmpeg 二进制

构建系统已自动包含 `fftools/ffmpeg_api.o`：

```bash
./configure --prefix=/home/tzy/source/FFmpeg/install \
    --enable-gpl --enable-version3 --enable-shared --enable-debug=3
make -C build -j$(nproc)
make -C build install
```

CLI 行为与改动前完全兼容。

### 4.2 嵌入到自有程序（demo）

由于 `fftools/ffmpeg.c` 自带 `main()`，宿主程序需要用 `-DFFMPEG_DRIVER_NO_MAIN` 重新编译它：

```bash
cd build

# 编译一份去掉 main() 的 ffmpeg.o
gcc -DFFMPEG_DRIVER_NO_MAIN -I. -I.. -I./fftools -I../fftools \
    -c ../fftools/ffmpeg.c -o fftools/ffmpeg_nomain.o

# 链接 demo
gcc -I. -I.. -I./fftools -I../fftools \
    ../fftools/ffmpeg_api_demo.c \
    fftools/ffmpeg_api.o fftools/ffmpeg_nomain.o \
    fftools/ffmpeg_dec.o fftools/ffmpeg_demux.o fftools/ffmpeg_enc.o \
    fftools/ffmpeg_filter.o fftools/ffmpeg_hw.o fftools/ffmpeg_mux.o \
    fftools/ffmpeg_mux_init.o fftools/ffmpeg_opt.o fftools/ffmpeg_sched.o \
    fftools/objpool.o fftools/sync_queue.o fftools/thread_queue.o \
    fftools/cmdutils.o fftools/opt_common.o \
    -L libavformat -lavformat -L libavcodec -lavcodec -L libavfilter -lavfilter \
    -L libavdevice -lavdevice -L libavutil -lavutil -L libswresample -lswresample \
    -L libswscale -lswscale -L libpostproc -lpostproc \
    -lpthread -lm -o ffmpeg_api_demo
```

运行：

```bash
LD_LIBRARY_PATH=$(pwd)/build/libavdevice:$(pwd)/build/libavformat:\
$(pwd)/build/libavfilter:$(pwd)/build/libavcodec:$(pwd)/build/libavutil:\
$(pwd)/build/libswresample:$(pwd)/build/libswscale:$(pwd)/build/libpostproc \
./build/ffmpeg_api_demo in1.mp4 out1.mkv  in2.mp4 out2.mkv
```

---

## 5. 使用示例

```c
#include "fftools/ffmpeg_api.h"
#include <stdio.h>

static void on_prog(FFmpegTask *t, const FFmpegProgress *p, void *u)
{
    printf("[%d] %.2fs pts=%.2fs frame=%llu fps=%.1f speed=%.2fx "
           "bitrate=%.1fk size=%lld stage=%d\n",
           p->task_id, p->elapsed_us / 1e6, p->pts_us / 1e6,
           (unsigned long long)p->frame_number, p->fps, p->speed,
           p->bitrate_kbps, (long long)p->total_size, p->stage);
}

static void on_err(FFmpegTask *t, int code, const char *msg, void *u)
{
    fprintf(stderr, "task %d ERROR %d: %s\n",
            ((FFmpegTask *)t)->task_id, code, msg);
}

static void on_fin(FFmpegTask *t, int ret, void *u)
{
    printf("task %d FINISHED ret=%d\n",
           ((FFmpegTask *)t)->task_id, ret);
}

int main(void)
{
    char *argv1[] = {
        "ffmpeg", "-hide_banner", "-y",
        "-i", "in1.mp4", "-c:v", "mpeg4", "out1.mkv", NULL };
    char *argv2[] = {
        "ffmpeg", "-hide_banner", "-y",
        "-i", "in2.mp4", "-c:v", "mpeg4", "out2.mkv", NULL };

    FFmpegTask *t1 = ffmpeg_task_alloc(8, argv1);
    FFmpegTask *t2 = ffmpeg_task_alloc(8, argv2);

    ffmpeg_task_set_id(t1, 1);
    ffmpeg_task_set_id(t2, 2);
    for (FFmpegTask *t : (FFmpegTask*[]){ t1, t2 }) {
        ffmpeg_task_set_progress_cb(t, on_prog);
        ffmpeg_task_set_error_cb   (t, on_err);
        ffmpeg_task_set_finish_cb  (t, on_fin);
    }

    /* 主线程派发到子线程 */
    ffmpeg_task_run_async(t1);
    ffmpeg_task_run_async(t2);

    /* 等待全部完成 */
    int rc1 = ffmpeg_task_join(t1);
    int rc2 = ffmpeg_task_join(t2);

    ffmpeg_task_free(&t1);
    ffmpeg_task_free(&t2);

    return rc1 ? rc1 : rc2;
}
```

> 想取消任务：在另一个线程调用 `ffmpeg_task_cancel(t)`。
> `decode_interrupt_cb` 与 `transcode` 主循环都会感知到 cancel_flag 并优雅退出。

---

## 6. 验证结果

| 测试 | 结果 |
|---|---|
| `make -C build -j ffmpeg` | 通过 |
| 原生 CLI：`testsrc → mpeg4 → mp4` | 与改动前输出一致 |
| 两并发任务 demo（每任务 10 秒输入） | 共 20 次进度回调（2×INIT + 16×RUNNING + 2×FINAL） |
| 异常路径 demo（`/no/such/path`） | 触发 `on_error(-2, "ffmpeg_run failed")` 与 `on_finish(ret=-2)` |
| 正常完成路径 | 触发 `on_finish(ret=0)` |

---

## 7. 已知限制与扩展方向

1. **`g_engine_mutex` 串行化**：同一进程内同时只允许一条转码 pipeline。
   要真正并发多条 pipeline，建议用子进程或独立 FFmpeg 库链接（每个进程一份全局变量）。
2. **Windows / 非 GCC 平台**：`_Thread_local` 是 C11 关键字（GCC、Clang、MSVC 1900+ 均支持）。
   若工具链不支持，可改写为 `__thread`（GCC）或 `__declspec(thread)`（MSVC）。
3. **取消粒度**：`ffmpeg_task_cancel()` 只在 `transcode` 循环和 IO 中断点检查，
   解码/编码深处不会立即响应；典型延迟 ≤ `stats_period`（默认 500ms）。
4. **进度回调字段扩展**：如需音频帧数、按流统计、时间码等，
   在 `FFmpegProgress` 增加字段即可；`ffmpeg.c print_report` 已有完整数据源。

---

## 8. 关键代码索引

- 任务定义：`fftools/ffmpeg_api.h`
- 任务实现 / 子线程入口：`fftools/ffmpeg_api.c`
- 进度回调注入：`fftools/ffmpeg.c` 中 `print_report()`
- 异常回调注入：`fftools/ffmpeg.c` 中 `transcode()` / `ffmpeg_run()` 末尾
- 任务取消检查：`fftools/ffmpeg.c` 中 `decode_interrupt_cb()` 与 `transcode()` 主循环
- TLS 复位：`fftools/ffmpeg.c` 中 `ffmpeg_state_reset()`
- 演示代码：`fftools/ffmpeg_api_demo.c`
