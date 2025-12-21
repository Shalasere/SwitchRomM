# RomM Switch Client – Design Notes

Audience: senior C++ devs working on `romm-switch-client/`. Snapshot of constraints, principles, and near-term plan.

## Goals
- Stable SDL2/libnx client for RomM: browse platforms/ROMs, queue downloads, split for FAT32/DBI, show progress, stay responsive.
- Network is `http://` only (no TLS); intended for trusted LAN or TLS terminated upstream.
 - Controls follow standard Nintendo layout: A = confirm/select, B = back, Y = queue view/add to queue, X = start/downloads, Plus = quit.

## Current Shape
- UI: `source/main.cpp` owns SDL init, config/API fetch, event/render loop, view state, text renderer, blocking cover loads.
- State: `include/romm/status.hpp` holds view enum, platform/ROM lists, queue, selections, progress atomics/strings, mutex.
- Input: `source/input.cpp` maps standard layout (A=Select/confirm, B=Back, Y=Queue view/add, X=Start downloads, Plus=Quit).
- HTTP/API: `source/api.cpp` hand-rolled HTTP (http-only, timeouts, chunked decode), JSON via `mini/json.hpp`, helpers to fetch platforms/ROMs/details and pick `.xci/.nsp`.
- Downloader: `source/downloader.cpp` worker thread; preflight HEAD/Range; stream one GET per ROM; split into 0xFFFF0000 parts; finalize single vs multi-part; archive bit set; mutex guarding added in worker.
- Config/logging: `.env`/JSON at `sdmc:/switch/romm_switch_client/`; leveled logging to SD + stdout/nxlink.
- Tests (host): Catch2 for URL parsing and chunked decode.

## Principles
- Single HTTP behavior, reused by API, covers, downloader.
- UI thread owns UI state; workers use atomics + messages, not shared container mutation.
- Downloads are transactional: temp dir, expected size, atomic finalize, manifestable.
- Controls/hints must match.
- Logs are useful but bounded.
- Docs mirror reality (http-only, DBI layout, queue semantics).

## Risks / Gaps
- Shared state: mutex exists and render locks it, but access is inconsistent (strings/vectors raced); enforce single policy and snapshots.
- Resume correctness: planResume() accepts non-contiguous parts; temp dirs can collide (title-only) and final names can overwrite; size uses metadata vs server inconsistently.
- HTTP robustness: downloader short-writes (send once), no chunked detection for streaming, duplicate HTTP stacks, no redirects/TLS/IPv6.
- UI perf: per-frame filesystem exists checks for every ROM; blocking cover load on render thread.
- Cancellation: stop relies on timeouts; no socket shutdown to unblock.
- Queue UX: no per-item state or dedupe; failed items dropped; `navStack` unused.
- Logging: no rotation; debug can flood SD; logger not thread-safe.
- Config: `apiToken`/`fat32Safe` unused; fast-fail on `https://` now enforced.

## Plan (near-term)
P0 - correctness/safety
- Enforce Status policy: all non-trivial fields under mutex; snapshot helpers; stop racy reads (e.g., currentDownloadIndex/title) and per-frame FS scans.
- Resume integrity: contiguous-only resume implemented; remaining: temp dirs keyed by rom/file id, avoid final-name collisions, use effective total size (Content-Length if present), optional hashes remain TODO (server doesn’t provide).
- Networking: loop send() in downloader preflight/stream, detect chunked for streaming and fail loudly, align server/metadata size use.
- Controls: canonical mapping A=Select, B=Back, Y=Queue, X=Download reflected in code, UI hints, docs; remove contradictory footers.

P1 - robustness/UX
- Unify HTTP client (shared parse/connect/stream); structured errors; enforce http-only at config load; track active socket and `shutdown()` on stop.
- Async cover loading with placeholder; keep SDL texture work on main thread.
- Queue UX: QueueItem state (Pending/Downloading/Completed/Failed), keep failed visible, dedupe enqueues, consistent back-nav.
- Cache on-disk completion state; update on download completion/manifest load/background scan instead of per-frame exists checks.

P2 - quality
- Manifest + optional hashes for downloads; validate resume beyond size-only; fsync temp/manifest.
- Log rotation/cap; thread-safe logger.
- UI polish (paging/search optional), tidy navStack, config flag cleanup.
- Expand tests: URL builder, file selection, downloader segmentation/resume (contiguity), input mapping, chunked detection/error.

## Constraints / Deployment
- Network: http only; use on trusted LAN or behind TLS-terminating proxy.
- Output layout: FAT32/DBI splits (0xFFFF0000) in `<download_dir>/temp/<safe>.tmp/`, finalized to `<download_dir>/<fsName>` or `<fsName>/00 01 ...` with archive bit.
- Logging path: `sdmc:/switch/romm_switch_client/log.txt`; keep `LOG_LEVEL=info` for normal use.
