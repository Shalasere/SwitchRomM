# Downloads

### How it works
- One streaming HTTP GET per ROM. We stop at Content-Length. If preflight sees `Accept-Ranges: bytes`, we resume partial data (including one partial part); otherwise the ROM restarts. **HTTP only; no TLS.** Use on trusted LAN or put TLS in front of RomM.
- Chunked transfer is not supported for streaming downloads; servers/proxies must send Content-Length. Redirects are not followed.
- Redirect failures now include the `Location` target and explicitly note that auth is not forwarded across hosts.
- Client-side split into FAT32/DBI parts: `0xFFFF0000` (00, 01, 02 ...) inside a temp dir when `fat32_safe=true`. If `fat32_safe=false`, the ROM stays as a single part. Each temp dir has a `manifest.json` with expected part sizes and which parts/partials are complete.
- Temps live under `<download_dir>/temp/<safe-12>_<id>.tmp/00.part`. After full download:
  - **Single-part**: rename/copy `00.part` to `<download_dir>/<Title or fsName>_<id>.<ext>` (ID-suffixed to avoid collisions); temp folder and manifest removed.
  - **Multi-part**: rename `.part` -> `00/01...`, move temp to `<download_dir>/<Title or fsName>_<id>.<ext>/`, set archive bit so DBI treats it as one title; manifest removed.
- File selection: fetch `/api/roms/{id}`, pick best `.xci/.nsp` from `files[]`, build `/api/roms/{id}/content/<fs_name>?file_ids=<id>`. No hidden-folder zips.

### HUD / badges
- Shows Current and Overall progress. Multi-file bundles show file progress (`N/M`). When all files are finalized, HUD switches to "Downloads complete".
- Badges per ROM: hollow (not queued), grey (queued), white (downloading), green (completed on disk), orange (resumable), red (failed).
- On startup, manifests in `temp/` load as Resumable (so you can retry), and final files on disk mark as Completed.
- Failures show a red "Failed: ." line. Short reads trigger a retry; if Range isn't supported that retry restarts the current ROM.
- Adding items while downloading recalculates overall bytes immediately; the overall % can dip when you enqueue mid-run (queue is not locked).
- Queue view surfaces a retained "Recent failures" summary even when active queue items are empty.
- Queue state is persisted to `sdmc:/switch/romm_switch_client/queue_state.json` so pending items survive restarts (deduped against completed-on-disk entries).

### Logging (see `logging.md`)
- Info: start/finish per ROM, finalize path, queue actions, failures.
- Debug: download heartbeats (~100?MB/10s), render traces. Enable with `log_level=debug`.

### Config knobs
- `download_dir` (default `sdmc:/romm_cache/switch`)
- `http_timeout_seconds` (default 30)
- `log_level` (`debug|info|warn|error`)

### Failure/cleanup
- Temp folders remain on failure/stop; safe to delete manually from `<download_dir>/temp/`.
- On restart, the app reuses complete parts and a single partial part using `manifest.json` (size-only validation today). Resume is enforced to contiguous parts only; any gap invalidates later parts. If Range is unavailable, the ROM restarts from zero.
- Preflight logs HTTP status; on tiny Content-Length or 404 we refresh metadata once, then fail fast.
- Free-space is checked up front and re-checked when rotating to each part file; failures are surfaced to UI/error diagnostics.
- Finalize logs the SD error string; single-part finalize falls back to copy-on-write if a rename fails.
- Slow WAN links: timeouts are bounded by `http_timeout_seconds` (also applied as stall detection during stream). Increase cautiously; too low can abort on jitter, too high can hang on dead links.

### TODO (known gaps)
- Resume validation is size-only; add hashes or stronger checks when feasible (server doesn’t provide hashes today).
- Optional: extra collision safeguards beyond title_id folders if future platforms need it.
- Redirects: currently fail with Location in the log; add optional follow with safe auth handling.
- HTTPS support when a TLS story is available.
