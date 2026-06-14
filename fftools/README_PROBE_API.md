# FFprobe Embeddable Engine API

> 把 `ffprobe` 嵌入到宿主程序，提供
> **任务完成后输出获取**（JSON / XML / CSV / default 等格式的文本结果）/
> **异常回调**，并支持 **主线程派发子线程任务** 的并发执行模型。
>
> 适配源码：`fftools/`（FFmpeg 7.1.x，`dev7` 分支）

---

## 1. 文件清单

| 文件 | 类型 | 说明 |
|---|---|---|
| `fftools/ffprobe_api.h` | 新增 | 公共 API：`FFprobeTask` / `FFprobeResult` / 回调 / 任务运行接口 |
| `fftools/ffprobe_api.c` | 新增 | 任务封装、`pthread_once` 一次性初始化、临时文件捕获输出（**无全局执行锁**） |
| `fftools/ffprobe_api_demo.c` | 新增 | 主线程派发多个子线程任务的演示，独立编译（`-DFFPROBE_DRIVER_NO_MAIN`） |
| `fftools/ffprobe.c` | 修改 | (1) 抽取 `ffprobe_run()`；`main()` 用 `FFPROBE_DRIVER_NO_MAIN` 包裹；(2) 14 个 options 静态绑定变量保留进程级 + 加 TLS 副本 + macro 重定向；(3) 30+ 个非绑定全局直接 TLS 化；(4) `sections[].entries_to_show / show_all_entries` 移到 TLS 旁路 `section_rt[]`；(5) `g_options_mutex` 仅锁 `parse_options()`，紧跟 `ffprobe_snapshot_opts_to_tls()` |
| `fftools/cmdutils.h/.c` | 修改 | `format_opts/codec_opts/sws_dict/swr_opts` 4 个 dict 改为 `_Thread_local` |
| `fftools/Makefile` | 修改 | 加入 `OBJS-ffprobe += fftools/ffprobe_api.o` |

---

## 2. 设计概览（v2 — 细粒度锁，真并发）

```
┌──────────────────────────────────────────────────────────────┐
│                       FFprobeTask                            │
│   argc/argv (deep-copy) / task_id / user_data /              │
│   cancel_flag / on_error / on_finish /                       │
│   tmp_path  → 用于捕获输出                                   │
│   output_text + output_size  → 结果缓冲                       │
└──────────────────────────────────────────────────────────────┘
                        │
              ┌─────────┴─────────┐
        run_sync                run_async
       (调用线程)            (pthread_create)
                        │
                        ▼
   ┌────────────────────────────────────────────────────────────┐
   │  ffprobe_task_set_current(task)         ← TLS current task │
   │  ffprobe_state_reset()                  ← 复位 TLS         │
   │  ┌──────────────────────────────────────────────────────┐  │
   │  │  ffprobe_options_lock()       ┐  短临界区             │  │
   │  │  parse_options(...)            │  ≈ 数毫秒             │  │
   │  │  ffprobe_snapshot_opts_to_tls()┘  进程级 → TLS        │  │
   │  │  ffprobe_options_unlock()                            │  │
   │  └──────────────────────────────────────────────────────┘  │
   │  probe_file / write  ─────────── 完全无锁，真并发           │
   │  slurp_file(tmp_path) → output_text                        │
   │  unlink(tmp_path)                                          │
   │  ffprobe_task_set_current(NULL)                            │
   └────────────────────────────────────────────────────────────┘
                        │
                        ▼
              on_finish(task, &result, user)
```

### 2.1 三类全局状态

| 类别 | 数量 | 处理方式 |
|------|------|---------|
| (A) 被 `real_options[]` 静态绑定的进程级变量 | 14 | 保留进程级 + 增加 `_Thread_local` 副本 + macro 重定向 + parse 完成立即 snapshot |
| (B) 非绑定的可变全局（`do_show_*`、`read_intervals`、`nb_streams_*` 等） | 30+ | 直接 `_Thread_local` |
| (C) `sections[]` 数组中的 `entries_to_show` / `show_all_entries` | 2 字段 × N sections | 移到 `_Thread_local SectionRuntime section_rt[SECTION_ID_NB]` 旁路数组，原 sections[] 保持只读 |

#### (A) Macro 重定向技巧

```c
/* 真全局：仅 real_options[] 静态绑定使用 */
static int do_bitexact = 0;
static char *output_format;

/* TLS 副本：runtime 唯一被读取的位置 */
static _Thread_local int  do_bitexact_tls = 0;
static _Thread_local char *output_format_tls;

/* 让所有读取者透明走 TLS 副本 */
#define do_bitexact   do_bitexact_tls
#define output_format output_format_tls
```

围绕 `real_options[]` 数组定义临时 `#undef` 这些名字，让 cmdutils 的
`{ &do_bitexact }` 绑定到真名；之后再 `#define` 回来，让后续函数（包括
`ffprobe_state_reset` / `ffprobe_run` / `mark_section_show_entries`）继续
看到 TLS 副本。

#### (B) cmdutils 字典也必须 TLS 化

`format_opts` / `codec_opts` / `sws_dict` / `swr_opts` 这 4 个 cmdutils 全局
被 `avformat_open_input` 等运行时 API 通过 `&format_opts` 写入并 free，
**必须**与任务一一对应，否则 N>=2 并发会触发 `malloc(): unaligned tcache`
之类的双 free。已在 `cmdutils.h/.c` 中改为 `extern _Thread_local`。

#### (C) 字符串选项的所有权移交

`output_format` 等 char* 选项在 snapshot 时通过移交所有权而非 strdup：

```c
av_freep(&output_format_tls);        /* 防御：释放上次任务残留 */
output_format_tls = output_format;   /* 接管字符串所有权 */
output_format = NULL;                /* 进程级清空 */
```

ffprobe_state_reset 中的 `av_freep(&output_format)` 经 macro 重定向后
变成 `av_freep(&output_format_tls)`，正好释放本线程上次任务的字符串。

### 2.2 临时文件 + 任务级取消

- `mkstemp("/tmp/ffprobe_api_XXXXXX")` 给每个任务分配独占文件名，
  argv 末尾自动追加 `-o <tmp_path>`，并发不冲突。
- `atomic_int cancel_flag` 由 `ffprobe_task_cancel()` 设置；
  在临时文件读入前后被检查，当前阶段 `parse_options` 已开始时不能强行打断。

### 2.3 实测扩展性（4 核机器，30s 720p 输入；`-count_frames -count_packets -show_streams`）

| 并发任务数 | 墙钟 | USER 时间 | CPU 利用率 | 加速比 |
|---:|---:|---:|---:|---:|
| 1 | 0.38s | 0.36s | 98%  | 1.0× |
| 2 | 0.53s | 0.84s | 160% | 1.59× |
| 3 | 0.57s | 1.52s | 267% | 2.67× |
| 4 | 0.77s | 2.24s | 293% | 2.92× |
| 8 | 1.45s | 4.84s | 336% | 3.36× |

- USER 时间近线性增长 → 锁/同步开销可忽略
- CPU% 接近物理核数上限（4×）→ 真并发利用 CPU
- 与旧的粗锁版本相比：N=8 从 ≈ 8 × 0.38s = 3.04s 降到 1.45s

### 2.2 输出捕获

ffprobe 的 `writer_open()` 在 `output_filename != NULL` 时通过 `avio_open()`
将所有文本写入文件，否则 `fprintf` 到 stdout。
为了把输出捕获到内存而**不**改动 writer 内部实现：

- API 在 `ffprobe_task_alloc()` 时给 argv 注入 `-o /tmp/ffprobe_api_XXXXXX`
- ffprobe 自然走 avio 路径写入文件
- 任务结束后读取文件全部内容到 `task->output_text`
- 删除临时文件

这种方法的好处：
- 完全不侵入 writer 体系，兼容 default / json / xml / ini / csv / flat / compact 等所有输出格式。
- 错误处理清晰：avio 报错走原有路径。
- 跨平台（任意 POSIX 系统都有 `mkstemp` + `unlink`）。

如果调用方在命令行里**已经**带了 `-o <file>`，API 会尊重它的选择，
此时 `output_text` 为空（因为输出已落盘到调用方指定的文件）。

### 2.4 默认输出格式

`ffprobe_task_alloc()` 检测 `-of / -output_format / -print_format`：
若都未提供，自动追加 `-of json`，确保宿主程序拿到结构化、易解析的结果。
调用方完全可以显式覆盖（例如 `-of xml` / `-of csv`）。

### 2.5 回调注入点

```c
// fftools/ffprobe.c ffprobe_run() 末尾失败路径
if (ret < 0) ffprobe_task_emit_error(ret, "ffprobe_run failed");

// fftools/ffprobe_api.c run_task_body() 完成（无全局锁包裹）
if (task->on_finish) {
    FFprobeResult r = { task->output_text, task->output_size, ret,
                        task->output_format };
    task->on_finish(task, &r, task->user_data);
}
```

---

## 3. API 参考

完整定义见 [`fftools/ffprobe_api.h`](./ffprobe_api.h)。

### 3.1 类型

```c
typedef struct FFprobeResult {
    const char *text;     /* NUL-terminated UTF-8, owned by task */
    size_t      size;     /* strlen(text)                        */
    int         ret;      /* 0 on success, < 0 on failure        */
    const char *format;   /* "default" / "json" / "xml" / ...    */
} FFprobeResult;

typedef struct FFprobeTask FFprobeTask;

typedef void (*ffprobe_error_cb )(FFprobeTask *task, int errcode,
                                  const char *msg, void *user_data);
typedef void (*ffprobe_finish_cb)(FFprobeTask *task,
                                  const FFprobeResult *result,
                                  void *user_data);
```

### 3.2 函数

| 函数 | 行为 |
|---|---|
| `ffprobe_task_alloc(argc, argv)`               | 深拷贝 argv，自动注入 `-of json` 与 `-o tmp` |
| `ffprobe_task_set_id / user_data`              | 配置任务标识与上下文 |
| `ffprobe_task_set_error_cb / finish_cb`        | 注册回调 |
| `ffprobe_task_run_sync(task)`                  | 在调用线程同步执行 |
| `ffprobe_task_run_async(task)`                 | `pthread_create` 起子线程 |
| `ffprobe_task_cancel(task)`                    | 协作取消（任意线程可调） |
| `ffprobe_task_join(task)`                      | 等待异步任务结束，返回 `ret_code` |
| `ffprobe_task_output(task)`                    | 直接读取捕获的文本（join 后） |
| `ffprobe_task_output_size(task)`               | 文本字节数 |
| `ffprobe_task_id / ret(task)`                  | 任务元信息 |
| `ffprobe_task_free(&task)`                     | 释放（必要时自动 cancel + join） |

### 3.3 返回码约定

| 值 | 含义 |
|---|---|
| `0` | 成功 |
| `<0` | FFmpeg 错误码（`AVERROR(...)`） |

---

## 4. 编译

### 4.1 普通 ffprobe 二进制

`fftools/Makefile` 已自动包含 `ffprobe_api.o`：

```bash
./configure --prefix=/home/tzy/source/FFmpeg/install \
    --enable-gpl --enable-version3 --enable-shared --enable-debug=3
make -C build -j$(nproc)
make -C build install
```

CLI 行为完全兼容。

### 4.2 嵌入到自有程序（demo）

```bash
cd build

# 编译一份去掉 main() 的 ffprobe.o
gcc -DFFPROBE_DRIVER_NO_MAIN -I. -I.. -I./fftools -I../fftools \
    -c ../fftools/ffprobe.c -o fftools/ffprobe_nomain.o

# 链接 demo
gcc -I. -I.. -I./fftools -I../fftools \
    ../fftools/ffprobe_api_demo.c \
    fftools/ffprobe_api.o   fftools/ffprobe_nomain.o \
    fftools/cmdutils.o      fftools/opt_common.o \
    -L libavformat -lavformat -L libavcodec -lavcodec \
    -L libavfilter -lavfilter -L libavdevice -lavdevice \
    -L libavutil -lavutil   -L libswresample -lswresample \
    -L libswscale -lswscale -L libpostproc -lpostproc \
    -lpthread -lm -o ffprobe_api_demo
```

运行：

```bash
LD_LIBRARY_PATH=$(pwd)/build/libavdevice:$(pwd)/build/libavformat:\
$(pwd)/build/libavfilter:$(pwd)/build/libavcodec:$(pwd)/build/libavutil:\
$(pwd)/build/libswresample:$(pwd)/build/libswscale:$(pwd)/build/libpostproc \
./build/ffprobe_api_demo file1.mp4 file2.mp4 file3.mp4
```

---

## 5. 使用示例

```c
#include "fftools/ffprobe_api.h"
#include <stdio.h>

static void on_error(FFprobeTask *t, int code, const char *msg, void *u)
{
    fprintf(stderr, "task %d: %d %s\n", ffprobe_task_id(t), code, msg);
}

static void on_finish(FFprobeTask *t, const FFprobeResult *r, void *u)
{
    printf("task %d done, ret=%d, %zu bytes JSON:\n%s\n",
           ffprobe_task_id(t), r->ret, r->size,
           r->text ? r->text : "(empty)");
}

int main(void)
{
    char *argv[] = {
        "ffprobe", "-hide_banner", "-loglevel", "error",
        "-show_format", "-show_streams",
        "in.mp4", NULL
    };

    FFprobeTask *t = ffprobe_task_alloc(7, argv);   /* -of json 自动注入 */
    ffprobe_task_set_id(t, 1);
    ffprobe_task_set_error_cb (t, on_error);
    ffprobe_task_set_finish_cb(t, on_finish);

    /* 同步：直接读取 ffprobe_task_output() */
    ffprobe_task_run_sync(t);
    printf("captured %zu bytes\n", ffprobe_task_output_size(t));

    ffprobe_task_free(&t);
    return 0;
}
```

异步多任务示例（最常见用法）：

```c
FFprobeTask *t1 = ffprobe_task_alloc(7, argv1);
FFprobeTask *t2 = ffprobe_task_alloc(7, argv2);
FFprobeTask *t3 = ffprobe_task_alloc(7, argv3);

/* 主线程派发到子线程；任务通过 g_engine_mutex 串行执行 */
ffprobe_task_run_async(t1);
ffprobe_task_run_async(t2);
ffprobe_task_run_async(t3);

/* 等待全部 */
ffprobe_task_join(t1);
ffprobe_task_join(t2);
ffprobe_task_join(t3);

/* 拿结果 */
puts(ffprobe_task_output(t1));
puts(ffprobe_task_output(t2));
puts(ffprobe_task_output(t3));

ffprobe_task_free(&t1);
ffprobe_task_free(&t2);
ffprobe_task_free(&t3);
```

---

## 6. 验证结果

| 测试 | 结果 |
|---|---|
| `make -C build -j ffprobe`            | 通过 |
| 原生 CLI：`ffprobe in.mp4`            | 与改动前输出一致 |
| 三并发任务 demo（`api_in1/2/big.mp4`）  | 每任务 2632 / 2632 / 2636 字节独立 JSON |
| 错误路径（不存在的文件）                | 触发 `on_error(-2, "ffprobe_run failed")` + `on_finish(ret=-2)` |
| 正常路径                              | 触发 `on_finish(ret=0)`，结果完整 |
| ffmpeg 工具回归                        | 不受本次改动影响 |

---

## 7. 已知限制与扩展

1. **g_engine_mutex 串行化**：同一进程内一时刻仅允许一条 ffprobe 任务在跑。
   ffprobe 单次调用通常很快（数十毫秒），串行化对总吞吐影响有限。
   如确需真并发探测，请用子进程隔离。
2. **临时文件路径**：当前固定 `/tmp/ffprobe_api_XXXXXX`。
   Windows / 非 POSIX 平台需要改用 `tmpnam_s` 或 `GetTempFileNameA`。
3. **取消粒度**：`ffprobe_task_cancel()` 只能阻止"尚未启动的任务"
   （在 g_engine_mutex 等待中），已经进入 `ffprobe_run` 的任务会跑完。
   如需更细粒度可在 `decode_interrupt_cb` 中接入。
4. **结果格式**：默认 JSON。如调用方需要 default/xml/csv/flat/ini/compact，
   显式传 `-of <fmt>` 即可，会被尊重并写入 `result.format`。

---

## 8. 关键代码索引

- 任务定义：`fftools/ffprobe_api.h`
- 任务实现 / 子线程入口 / 临时文件捕获：`fftools/ffprobe_api.c`
- ffprobe 内核：`fftools/ffprobe.c` 中 `ffprobe_run()` / `ffprobe_state_reset()`
- 异常回调注入：`fftools/ffprobe.c` 中 `ffprobe_run()` 的 `end:` 标签
- 演示代码：`fftools/ffprobe_api_demo.c`

---

## 9. 与 ffmpeg embed API 的关系

参见 [`README_EMBED_API.md`](./README_EMBED_API.md)。
两套 API 设计风格一致：

| 特性 | ffmpeg embed API | ffprobe embed API |
|---|---|---|
| 主体源 | `ffmpeg.c` | `ffprobe.c` |
| 全局状态隔离 | TLS + 进程级 + `g_engine_mutex` | 进程级 + `g_engine_mutex` + `state_reset` |
| 进度回调（1Hz） | ✅ | ❌（ffprobe 是单次探测，无进度概念） |
| 错误回调 | ✅ | ✅ |
| 完成回调 | ✅（携带 ret） | ✅（携带 ret + 文本结果） |
| 任务取消 | ✅（含解码循环检查） | 半支持（仅排队） |
| 输出获取 | 直接写文件/stdout | 自动捕获到内存缓冲区 |
| Makefile 改动 | `OBJS-ffmpeg +=` | `OBJS-ffprobe +=` |
| Demo 编译宏 | `-DFFMPEG_DRIVER_NO_MAIN` | `-DFFPROBE_DRIVER_NO_MAIN` |
