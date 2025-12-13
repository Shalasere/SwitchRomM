# RomM Switch Client – Design Notes

Audience: senior C++ devs working on `romm-switch-client/`. This captures current constraints, principles, and the near-term plan.

## Goals
- Stable SDL2/libnx client for RomM: browse platforms/ROMs, queue downloads, split for FAT32/DBI, show progress, stay responsive.
- Keep network plain `http://` (TLS not implemented); optimize for trusted LAN or TLS terminated upstream.

## Current Shape (snapshot)
- UI: `source/main.cpp` owns SDL init, config/API fetch, event/render loop, view state, text renderer, blocking cover loads.
- State: `include/romm/status.hpp` holds view enum, platform/ROM lists, queue, selections, progress atomics, strings (errors/titles), mutex.
- Input: `source/input.cpp` maps Nintendo layout (currently A=Select, B=Back, X=Queue, Y=Start Download, Plus=Quit).
- HTTP/API: `source/api.cpp` hand-rolled HTTP (http-only, timeouts, chunked decode), JSON via `mini/json.hpp`, helpers to fetch platforms/ROMs/details and pick `.xci/.nsp`.
- Downloader: `source/downloader.cpp` worker thread; preflight HEAD/Range; stream one GET per ROM; split into 0xFFFF0000 parts; finalize single vs multi-part; set archive bit; mutex guarding added in worker.
- Config/logging: `.env`/JSON at `sdmc:/switch/romm_switch_client/`; levelled logging to SD + stdout/nxlink.
- Tests (host): Catch2 for URL parsing and chunked decode.

## Principles
- Single source of truth for HTTP behavior; reuse for API, covers, downloader.
- UI thread owns UI state; background threads communicate via atomics + messages, not shared container mutation.
- Downloads are transactional: temp dir, expected size, atomic finalize, manifestable.
- Controls and on-screen hints must match; surprises are bugs.
- Logs are useful but bounded; debug should be intentional.
- Docs mirror reality (http-only, DBI layout, queue semantics).

## Risks / Gaps
- Data races: UI and worker mutate `Status` containers/strings; render now locks but worker/UI sharing needs a clearer pattern.
- Input/hints mismatch historically; mapping now flipped to A=Select/B=Back but hints should be verified.
- Blocking cover load on render thread.
- Duplicate HTTP stacks (API vs downloader); no TLS, no redirects.
- Cancellation: stop relies on timeouts; no socket shutdown to unblock.
- Queue UX: no per-item state or dedupe; failed items dropped; `navStack` unused.
- Logging: no rotation; debug can flood SD.
- Config: `apiToken`/`fat32Safe` unused; no fast fail on `https://` (now enforced at load).

## Plan (near-term)
P0 – correctness/safety
- Guard `Status` non-atomics with a mutex; render from snapshots. Restrict worker to atomics + messages.
- Align controls and hints (now A=Select/B=Back).
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
