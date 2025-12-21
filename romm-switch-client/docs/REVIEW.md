# RomM Switch Client - Technical Review

Quick review of the C++17/libnx/SDL2 codebase: architecture, flows, risks, and pointers for contributors.  
Last updated: 2025-12-15 (controls standardized A=Select/B=Back/Y=Queue/X=Download; resume/HTTP gaps documented).

## Architecture snapshot
- **Entry/UI**: `source/main.cpp` - SDL init, config load, API fetch, input handling, view transitions, rendering.
- **State**: `include/romm/status.hpp` (UI/download state), `include/romm/models.hpp` (Platform/Game), `include/romm/config.hpp` (server/auth/download_dir/fat32_safe). Config keys in `docs/config.md`.
- **Input**: `source/input.cpp` - SDL controller mapping (A=Select, B=Back, Y=Queue, X=Start Downloads, Plus=Quit; Nintendo layout) with debounce; raw JOY ignored; log_level from `.env`. Controls in `docs/controls.md`.
- **Data/API**: `source/api.cpp` - minimal HTTP (HTTP-only, Basic auth), JSON via `mini/json.hpp`; fetches `/api/roms/{id}`, picks `.xci/.nsp` via `file_id`, builds `/content/<fs_name>?file_ids=<id>`; Range preflight.
- **Covers**: cover_url parsed, relative URLs prefixed; reliability in DETAIL view still TODO.
- **Downloader**: `source/downloader.cpp` - background worker, FAT32 parts (0xFFFF0000), temp dirs from title/fsName; skips complete parts, deletes partials; single-part rename/copy fallback; multi-part archive bit; queue not locked; resume keeps counters aligned; one GET per ROM with large recv buffer.

## Logic flows (per view)
- **PLATFORMS**: Select (A) fetches ROMs; Back (B) ignored; Y opens QUEUE (prevQueueView=PLATFORMS); Plus quits.
- **ROMS**: Select (A) -> DETAIL; Back (B) -> PLATFORMS; Y -> QUEUE (prevQueueView=ROMS); D-pad scroll with acceleration.
- **DETAIL**: Shows ROM metadata; Select (A) enqueues and switches to QUEUE; Back (B) -> ROMS; Y -> QUEUE (prevQueueView=DETAIL).
- **QUEUE**: Lists queued ROMs; X starts downloads -> DOWNLOADING; Back returns to prevQueueView; Plus quits; empty shows "Queue empty." or "All downloads complete."
- **DOWNLOADING**: Shows global progress, percent/bytes; "Connecting..." when no data yet; failure text if lastDownloadFailed; B -> QUEUE; Plus quits (stops worker).
- **ERROR**: Set on API failure; Quit exits.

## Issues (bugs/risks)
Severity: [H]=High, [M]=Medium, [L]=Low. File refs approximate.
- [H] Thread safety (`source/downloader.cpp`, `source/main.cpp`): `Status` shared; strings/vectors sometimes read/written without lock (e.g., currentDownloadIndex/title). **Fix**: guard all non-atomic fields with `Status::mutex` or move to an event queue; keep counters atomic; snapshot under lock.
- [H] Resume integrity (`source/manifest.cpp`, `source/downloader.cpp`): contiguity enforced in planResume; validation still size-only (hashes TODO; server doesn’t provide), temp dirs now include rom/file IDs but final names can still overwrite. **Fix**: add stronger validation (hash when feasible), collision-safe final names, size-consistent progress (use Content-Length if present).
- [H] HTTP robustness (`source/api.cpp`, `source/downloader.cpp`): HTTP-only, no TLS. Downloader send() does not loop (short-write risk); streaming assumes Content-Length and does not detect chunked; duplicate HTTP stacks. **Fix**: loop send in downloader, detect chunked and fail loudly, unify HTTP handling; optional TLS/redirects later.
- [M] Archive bit: best-effort for multi-part folders; single-part emits flat file. Residual risk: SD errors on finalize.
- [M] Resume granularity (`source/downloader.cpp`): Size-only validation; only full parts resume; partials deleted. **Fix**: manifest with expected sizes/count (and optional checksum), validate sizes, allow mid-part resume once contiguous enforcement is in place.
- [M] Error handling/retries (`source/downloader.cpp`): Limited retry/backoff; failures drop items and continue. **Fix**: bounded retries/backoff per ROM with user-visible status; keep failed item info in UI.
- [M] UI feedback (`source/main.cpp` DOWNLOADING): Limited status when stalled; last status/error not shown until failure. **Fix**: show last range/error, per-ROM progress; differentiate "no data yet" vs "stalled."
- [M] Free-space handling (`source/downloader.cpp`): Single pre-check; no re-check per part; write errors not surfaced. **Fix**: re-check before each part; handle write errors and report to UI.
- [M] Config/auth (`include/romm/config.hpp`, `source/api.cpp`): Only Basic auth; `apiToken` unused; assumes `http://`. **Fix**: support token header, validate scheme/port; surface auth errors.
- [L] Logging volume/threading: Debug-only heartbeats/file listings; no rotation; logger opens file per line and is not synchronized. **Fix**: optional size cap/rotation; thread-safe sink.
- [L] Structure/style (`source/main.cpp`): Large renderStatus and input switch mix concerns. **Fix**: split per-view render functions/controllers; wrap sockets/files in RAII.
- [L] Data/UI fidelity: Model titles are sanitized to ASCII on parse (UTF-8 lost); download/cover URLs only encode spaces and may not be absolutized if provided relative; cover loader is latest-only (drops queued covers). **Fix**: keep UTF-8 in model and sanitize at render; use full urlEncode + absolutize download_url/cover URLs; document latest-wins cover loader or add queue; add redirect/IPv6/trailer handling if needed.

## File notes
- `source/main.cpp`: SDL lifecycle; config/API fetch; input loop maps Action -> state; view transitions; renderStatus draws all views; download view shows global progress/failure; queue view shows completion when empty after success.
- `source/input.cpp`: Controller mapping with debounce; ignores JOY events. A=Select, B=Back, Y=Queue view/add, X=Start Download, Plus=Quit.
- `source/api.cpp`: Minimal HTTP (no TLS), Basic auth; parses platforms/ROMs; fetches DetailedRom files[]; builds download URLs via `file_ids` (raw XCI/NSP, no hidden_folder zip).
- `source/downloader.cpp`: Background worker; parts at 0xFFFF0000; temp dir from truncated title/fsName; skips complete parts, deletes partials; sequential per ROM; queue items removed on completion/failure; finalize renames `.part` to `00/01/...` and moves temp dir to final name; sets concatenation/archive bit for multi-part; single-part rename has copy fallback; limited retries/backoff; no TLS.

## Conventions
- **RAII**: Raw sockets/files; manual close. RAII wrappers recommended.
- **Const-correctness**: Mostly fine; could increase const usage for helpers/params.
- **Error handling**: Bool+string; fragile for HTTP/JSON. Consider richer error codes.
- **Concurrency**: Needs synchronization (see above).
- **Naming/clarity**: Generally clear; global debug counters could be scoped.
- **Rendering**: `renderStatus` is large; split per view.

## Suggested next steps (roadmap)
- **Thread safety**: Finish guarding all shared `Status` fields; consider an event queue for worker->UI updates; keep only counters atomic.
- **HTTP/TLS**: Keep the unified streaming path; add TLS or document LAN-only; improve error reporting and retry/backoff.
- **Resume robustness**: Add per-ROM manifest (sizes/hashes), mid-part resume, and explicit failed-item retention in the queue UI.
- **Download UX**: Show per-ROM state (pending/downloading/failed/done), surface last error in DOWNLOADING, add free-space re-checks before each part, avoid per-frame filesystem scans (cache completion state).
- **Covers/UI**: Keep async cover loading; placeholder when absent; avoid blocking render thread.
- **Plutonium UI refactor**: Explore migrating SDL UI to Plutonium for cleaner view separation and Switch-native widgets, preserving current control mapping.
- **Logging**: Add optional size cap/rotation; keep debug heartbeats behind log_level.
- **Refactors**: Split `renderStatus` into per-view helpers; wrap sockets/files in RAII (helpers exist); keep controls fixed per `docs/controls.md`.

## Credits / Inspiration
- Concepts and flow informed by the RomM muOS client: https://github.com/rommapp/muos-app
