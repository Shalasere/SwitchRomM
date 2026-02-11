# RomM Switch Client - Technical Review

Quick review of the C++17/libnx/SDL2 codebase: architecture, flows, risks, and pointers for contributors.  
Last updated: 2026-02-11 (controls positional: A=Back/B=Select/Y=Queue/X=Start, Minus=Search, R=Diagnostics gate; revision-keyed ROM search/filter/sort index; diagnostics view/export; config schema_version migration support for JSON; per-file download HUD progress and queue failure retention summary; queue snapshot restore across restarts; worker->UI status event channel; UTF-8 title preservation with ASCII folding at render/search; free-space recheck on part rotation; resume contiguity enforced; title_id folders; chunked fail-fast; redirect logged with Location; speed test optional; FAT32 splitting toggle).

## Architecture snapshot
- **Entry/UI**: `source/main.cpp` - SDL init, config load, API fetch, input handling, view transitions, rendering.
- **State**: `include/romm/status.hpp` (UI/download state), `include/romm/models.hpp` (Platform/Game), `include/romm/config.hpp` (server/auth/download_dir/fat32_safe + parsed config schema version). Config keys in `docs/config.md`.
- **Input**: `source/input.cpp` - SDL controller mapping reversed (A=Back, B=Select, Y=Queue, X=Start Downloads, Minus=Search, R=Diagnostics, Plus=Quit; positional mode) with debounce; raw JOY ignored. Controls in `docs/controls.md`.
- **Data/API**: `source/api.cpp` - minimal HTTP (HTTP-only, Basic auth), JSON via `mini/json.hpp`; fetches `/api/roms/{id}`, ingests full files[]; builds download URLs via `file_ids`; Range preflight; cover URLs now fully encoded/absolutized; redirects logged with Location but not followed.
- **Covers**: cover_url parsed/absolutized; loader is latest-wins (single-slot) by design.
- **Downloader**: `source/downloader.cpp` - background worker; FAT32 parts (0xFFFF0000) when `fat32_safe=true`, otherwise single-part; temp dirs under `<download_dir>/temp/<platform>/<rom>/<file>/...`; skips complete parts, deletes partials; single-part rename/copy fallback; multi-part archive bit; per-ROM folder `title_id`; resume keeps counters aligned; bundle_best selects best dir group; avoid tokens supported via platform prefs; per-file relative paths honored.

## Logic flows (per view)
- **PLATFORMS**: Select (A) fetches ROMs; Back (B) ignored; Y opens QUEUE (prevQueueView=PLATFORMS); R opens DIAGNOSTICS; Plus quits.
- **ROMS**: Select (A) -> DETAIL; Back (B) -> PLATFORMS; Y -> QUEUE (prevQueueView=ROMS); Minus opens search keyboard; D-pad Left/Right cycles filter/sort; D-pad Up/Down scroll with acceleration.
- **DETAIL**: Shows ROM metadata; Select (A) enqueues and switches to QUEUE; Back (B) -> ROMS; Y -> QUEUE (prevQueueView=DETAIL).
- **QUEUE**: Lists queued ROMs; X starts downloads -> DOWNLOADING; Back returns to prevQueueView; Plus quits; empty shows "Queue empty." or "All downloads complete."
- **DOWNLOADING**: Shows global progress, percent/bytes/MBps; SPD header when speed test url is set; "Connecting..." when no data yet; failure text if lastDownloadFailed; B -> QUEUE; Plus quits (stops worker).
- **DIAGNOSTICS**: Shows config summary, server reachability probe, SD free space, queue/history stats, and last error; Select (A) exports a support summary to log; R refreshes probe; Back (B) returns to previous view. Launch is intentionally gated to PLATFORMS.
- **ERROR**: Set on API failure; Quit exits.

## Issues (bugs/risks)
Severity: [H]=High, [M]=Medium, [L]=Low. File refs approximate.
- [H] Thread safety (`source/downloader.cpp`, `source/main.cpp`): `Status` shared; most hotspots now under mutex/snapshot, but watch for regressions. **Fix**: guard all non-atomic fields with `Status::mutex` or move to an event queue; keep counters atomic; snapshot under lock.
- [H] Resume integrity (`source/manifest.cpp`, `source/downloader.cpp`): contiguity enforced; validation still size-only (hashes TODO; server doesn't provide). **Fix**: add stronger validation (hash when feasible); keep manifest/platform slug consistent.
- [H] HTTP robustness (`source/api.cpp`, `source/downloader.cpp`): HTTP-only, no TLS. Downloader send() now loops; streaming explicitly fails on chunked TE; redirects are not followed (logged with Location). **Fix**: unify HTTP handling; optional TLS/redirect follow with safe auth rules; decide on chunked streaming support.
- [M] Archive bit: best-effort for multi-part folders; single-part emits flat file. Residual risk: SD errors on finalize.
- [M] Resume granularity (`source/downloader.cpp`): Size-only validation; only full parts resume; partials deleted. **Fix**: manifest with expected sizes/count (and optional checksum), allow mid-part resume once contiguous enforcement is in place.
- [M] Error handling/retries (`source/downloader.cpp`): Limited retry/backoff; failures drop items and continue. **Fix**: bounded retries/backoff per ROM with user-visible status; keep failed item info in UI.
- [M] UI feedback (`source/main.cpp` DOWNLOADING/QUEUE): Shows MBps and per-file bundle progress with retained recent failure summaries; still limited when stalled. **Fix**: show last range/error and better stalled-state messaging.
- [M] Free-space handling (`source/downloader.cpp`): Up-front and per-part checks are in place (best effort if statvfs unavailable); write errors surface to UI. **Fix**: optional hard-fail policy when free-space telemetry is unavailable.
- [M] Config/auth (`include/romm/config.hpp`, `source/api.cpp`): Only Basic auth; `apiToken` unused; assumes `http://`. **Fix**: support token header, validate scheme/port; surface auth errors.
- [L] Logging volume/threading: Debug-only heartbeats/file listings; rotation and mutexed sink are in place. Further tuning is optional (reduce sinks or verbosity).
- [L] Structure/style (`source/main.cpp`): Large renderStatus and input switch mix concerns. **Fix**: split per-view render functions/controllers; wrap sockets/files in RAII.
- [L] Data/UI fidelity: UTF-8 model title preservation now lands in model and folds at render/search; non-Latin scripts still fall back to `?` with the current bitmap glyph set. Cover loader remains latest-only (drops queued covers). **Fix**: add broader glyph coverage or optional TTF fallback; document latest-wins cover loader or add queue; add redirect follow/IPv6/trailer handling if needed. SPD speed test runs once at startup if URL set; optional.

## File notes
- `source/main.cpp`: SDL lifecycle; config/API fetch; input loop maps Action -> state; revision-keyed ROM indexing (search/filter/sort) and diagnostics probe/export; renderStatus draws all views; download view shows global+per-file progress/failure; queue view shows completion and retained recent failures.
- `source/input.cpp`: Controller mapping with debounce; ignores JOY events. SDL controls reversed A=Back, B=Select, Y=Queue view/add, X=Start Download, Minus=Search, R=Diagnostics, Plus=Quit (UI footers match mapping).
- `source/api.cpp`: Minimal HTTP (no TLS), Basic auth; parses platforms/ROMs; fetches DetailedRom files[]; builds download URLs via `file_ids`; cover/download URLs encoded/absolutized; redirects logged but not followed.
- `source/downloader.cpp`: Background worker; parts at 0xFFFF0000 when `fat32_safe=true`, otherwise single-part; temp dir under `<download_dir>/temp/<platform>/<rom>/<file>/...`; skips complete parts; sequential per bundle; queue items removed on completion/failure; finalize renames `.part` to `00/01/...` and moves temp dir to `title_id` folder; sets concatenation/archive bit for multi-part; single-part rename has copy fallback; limited retries/backoff; HTTP-only, no TLS/redirects/chunked streaming.

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
