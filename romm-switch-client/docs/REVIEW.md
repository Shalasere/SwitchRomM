# RomM Switch Client - Technical Review

Quick review of the C++17/libnx/SDL2 codebase: architecture, flows, risks, and pointers for contributors.  
Last updated: 2025-12-12 (file_id downloads, macron UTF-8 fix, resume counter fixes, archive-bit finalize, logging dirs ensured, docs synced, host tests for HTTP parsing/chunked decoding).

## Architecture snapshot
- **Entry/UI**: `source/main.cpp` - SDL init, config load, API fetch, input handling, view transitions, rendering.
- **State**: `include/romm/status.hpp` (UI/download state), `include/romm/models.hpp` (Platform/Game), `include/romm/config.hpp` (server/auth/download_dir/fat32_safe). Config keys in `docs/config.md`.
- **Input**: `source/input.cpp` - SDL controller mapping (A=Back, B=Select, X=Queue, Y=Start Download, Plus=Quit; Nintendo layout) with debounce; raw JOY ignored; log_level from `.env`. Controls in `docs/controls.md`.
- **Data/API**: `source/api.cpp` - minimal HTTP (HTTP-only, Basic auth), JSON via `mini/json.hpp`; fetches `/api/roms/{id}`, picks `.xci/.nsp` via `file_id`, builds `/content/<fs_name>?file_ids=<id>`; Range preflight.
- **Covers**: cover_url parsed, relative URLs prefixed; reliability in DETAIL view still TODO.
- **Downloader**: `source/downloader.cpp` - background worker, FAT32 parts (0xFFFF0000), temp dirs from title/fsName; skips complete parts, deletes partials; single-part rename/copy fallback; multi-part archive bit; queue not locked; resume keeps counters aligned; one GET per ROM with large recv buffer.

## Logic flows (per view)
- **PLATFORMS**: Select (B) fetches ROMs; Back (A) ignored; Y opens QUEUE (prevQueueView=PLATFORMS); Plus quits.
- **ROMS**: Select (B) -> DETAIL; Back (A) -> PLATFORMS; Y -> QUEUE (prevQueueView=ROMS); D-pad scroll with acceleration.
- **DETAIL**: Shows ROM metadata; Select (B) enqueues and switches to QUEUE; Back (A) -> ROMS; Y -> QUEUE (prevQueueView=DETAIL).
- **QUEUE**: Lists queued ROMs; Y starts downloads -> DOWNLOADING; Back returns to prevQueueView; Plus quits; empty shows "Queue empty." or "All downloads complete."
- **DOWNLOADING**: Shows global progress, percent/bytes; "Connecting..." when no data yet; failure text if lastDownloadFailed; B -> QUEUE; Plus quits (stops worker).
- **ERROR**: Set on API failure; Quit exits.

## Issues (bugs/risks)
Severity: [H]=High, [M]=Medium, [L]=Low. File refs approximate.
- [H] Thread safety (`source/downloader.cpp`, `source/main.cpp`): `Status` mutated by worker and UI with no locks. **Fix**: add mutex/message-passing; keep progress in atomics.
- [H] HTTP robustness (`source/api.cpp`, `source/downloader.cpp`): Minimal HTTP client (no TLS). Range preflight present; JSON parsing basic. **Fix**: optional TLS, richer status/body handling, tighter resume validation.
- [M] Archive bit: now set best-effort for multi-part folders; single-part emits flat file. Residual risk: SD errors on finalize.
- [M] Resume granularity (`source/downloader.cpp`): Only full parts resume; no manifest/checksum; partials deleted. **Fix**: manifest with expected sizes/count, validate sizes, optional checksum; allow mid-part resume. Current counters stay aligned after retries.
- [M] Error handling/retries (`source/downloader.cpp`): Limited retry/backoff; failures drop items and continue. **Fix**: bounded retries/backoff per ROM with user-visible status; keep failed item info in UI.
- [M] UI feedback (`source/main.cpp` DOWNLOADING): Limited status when stalled; last status/error not shown until failure. **Fix**: show last range/error, per-ROM progress; differentiate "no data yet" vs "stalled."
- [M] Free-space handling (`source/downloader.cpp`): Single pre-check; no re-check per part; write errors not surfaced. **Fix**: re-check before each part; handle write errors and report to UI.
- [M] Config/auth (`include/romm/config.hpp`, `source/api.cpp`): Only Basic auth; `apiToken` unused; assumes `http://`. **Fix**: support token header, validate scheme/port; surface auth errors.
- [L] Logging volume: Debug-only heartbeats/file listings; no rotation. **Fix**: optional size cap/rotation; log dir creation best-effort via `ensureDirectory`.
- [L] Structure/style (`source/main.cpp`): Large renderStatus and input switch mix concerns. **Fix**: split per-view render functions/controllers; wrap sockets/files in RAII.

## File notes
- `source/main.cpp`: SDL lifecycle; config/API fetch; input loop maps Action -> state; view transitions; renderStatus draws all views; download view shows global progress/failure; queue view shows completion when empty after success.
- `source/input.cpp`: Controller mapping with debounce; ignores JOY events. A=Back, B=Select, X=OpenQueue, Y=StartDownload, Plus=Quit.
- `source/api.cpp`: Minimal HTTP (no TLS), Basic auth; parses platforms/ROMs; fetches DetailedRom files[]; builds download URLs via `file_ids` (raw XCI/NSP, no hidden_folder zip).
- `source/downloader.cpp`: Background worker; parts at 0xFFFF0000; temp dir from truncated title/fsName; skips complete parts, deletes partials; sequential per ROM; queue items removed on completion/failure; finalize renames `.part` to `00/01/...` and moves temp dir to final name; sets concatenation/archive bit for multi-part; single-part rename has copy fallback; limited retries/backoff; no TLS.

## Conventions
- **RAII**: Raw sockets/files; manual close. RAII wrappers recommended.
- **Const-correctness**: Mostly fine; could increase const usage for helpers/params.
- **Error handling**: Bool+string; fragile for HTTP/JSON. Consider richer error codes.
- **Concurrency**: Needs synchronization (see above).
- **Naming/clarity**: Generally clear; global debug counters could be scoped.
- **Rendering**: `renderStatus` is large; split per view.

## Suggested next steps
- Add mutex/message queue for worker -> UI state; keep progress atomic.
- Implement retry/backoff and better HTTP status/body handling; add "last status" text to DOWNLOADING.
- Add manifest-based resume and part size validation; optional checksum.
- Maintain archive-bit best-effort on multi-part folders; surface failures in UI/logs.
- Re-check free space per part; handle write errors.
- Refactor renderStatus into per-view functions; wrap sockets/files in RAII helpers.
- Expand inline code comments (partially done) to orient new contributors.

## Credits / Inspiration
- Concepts and flow informed by the RomM muOS client: https://github.com/rommapp/muos-app
