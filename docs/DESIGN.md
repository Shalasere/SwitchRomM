# RomM Switch Client – Design Notes

Audience: senior C++ devs working on `romm-switch-client/`. Snapshot of constraints, principles, and near-term plan.

## Goals
- Stable SDL2/libnx client for RomM: browse platforms/ROMs, queue downloads, split for FAT32/DBI, show progress, stay responsive.
- Network is `http://` only (no TLS); intended for trusted LAN or TLS terminated upstream.

## Current Shape
- UI: `source/main.cpp` owns SDL init, config/API fetch, event/render loop, view state, text renderer, blocking cover loads.
- State: `include/romm/status.hpp` holds view enum, platform/ROM lists, queue, selections, progress atomics/strings, mutex.
- Input: `source/input.cpp` maps Nintendo layout (A=Back, B=Select, X=Queue view/start downloads, Y=Open queue, Plus=Quit).
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
- Shared state: mutex exists and render locks it, but we still need a clean snapshot/message pattern between UI and worker to avoid long-held locks.
- Blocking cover load on render thread.
- Duplicate HTTP stacks (API vs downloader); no TLS, no redirects.
- Cancellation: stop relies on timeouts; no socket shutdown to unblock.
- Queue UX: no per-item state or dedupe; failed items dropped; `navStack` unused.
- Logging: no rotation; debug can flood SD.
- Config: `apiToken`/`fat32Safe` unused; fast-fail on `https://` now enforced.

## Plan (near-term)
P0 – correctness/safety
- Shared state: finish snapshot/message pattern (render from snapshots, worker uses atomics + messages).
- Controls/hints aligned (A=Back/B=Select).
- Fix any broken DETAIL text rendering.

P1 – robustness/UX
- Unify HTTP client (shared parse/connect/stream); structured errors; enforce http-only at config load; track active socket and `shutdown()` on stop.
- Async cover loading with placeholder; keep SDL texture work on main thread.
- Queue UX: QueueItem state (Pending/Downloading/Completed/Failed), keep failed visible, dedupe enqueues, consistent back-nav.

P2 – quality
- Manifest + optional hashes for downloads; validate resume beyond size-only; fsync temp/manifest.
- Log rotation/cap.
- UI polish (paging/search optional), tidy navStack, config flag cleanup.
- Expand tests: URL builder, file selection, downloader segmentation/resume (simulated), input mapping.

## Constraints / Deployment
- Network: http only; use on trusted LAN or behind TLS-terminating proxy.
- Output layout: FAT32/DBI splits (0xFFFF0000) in `<download_dir>/temp/<safe>.tmp/`, finalized to `<download_dir>/<fsName>` or `<fsName>/00 01 ...` with archive bit.
- Logging path: `sdmc:/switch/romm_switch_client/log.txt`; keep `LOG_LEVEL=info` for normal use.
