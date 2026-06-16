# AGENTS.md

## Cursor Cloud specific instructions

This is **FFmpeg** — a C codebase built with a custom `./configure` script and GNU Make.
There is no package manager; the build is driven directly by `configure` + `make`.

### Services / products
There is no long-running server. The build produces CLI tools at the repo root:
`ffmpeg` (transcode/convert), `ffprobe` (inspect media), and the libraries
(`libav*`, `libsw*`). "Running the app" means invoking these binaries.

### Build (development)
- Configure once: `./configure` (default LGPL build, no external codec deps).
  Re-run `./configure` only when you change build options.
- Build: `make -j$(nproc)` (~2-3 min cold on this VM). Binaries land at the repo
  root as `./ffmpeg` and `./ffprobe`. Incremental rebuilds after edits are just `make -j$(nproc)`.
- `nasm` is required on x86_64 for the assembly-optimized code paths; the update
  script installs it.

### Lint
- Uses `pre-commit` with the repo config: `pre-commit run -c .forgejo/pre-commit/config.yaml --all-files`.
- `pre-commit` is installed to `~/.local/bin` (ensure it is on `PATH`). The first
  run downloads hook environments (needs network); later runs are cached/offline.

### Test (FATE)
- Run the sample-independent suite with: `make fate -j$(nproc)`.
- The **full** FATE suite needs the external fate-suite samples
  (`make fate-rsync SAMPLES=$PWD/fate-suite` then
  `make fate SAMPLES=$PWD/fate-suite -j$(nproc)`). Syncing samples needs network
  and is large; only do it if full coverage is required.
- Running FATE leaves untracked test-helper binaries under `*/tests/` (e.g.
  `libswresample/tests/swresample_resample_realloc`). These are build artifacts —
  do not commit them.

### Quick sanity check (hello-world)
```
./ffmpeg -f lavfi -i testsrc=duration=3:size=320x240:rate=25 -f lavfi -i sine=frequency=440:duration=3 -c:v mpeg4 -c:a aac /tmp/hello.mp4
./ffprobe /tmp/hello.mp4
```
