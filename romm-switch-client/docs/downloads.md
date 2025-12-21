# Downloads

### How it works
- One streaming HTTP GET per ROM. We stop at Content-Length. If preflight sees `Accept-Ranges: bytes`, we resume partial data (including one partial part); otherwise the ROM restarts. **HTTP only; no TLS.** Use on trusted LAN or put TLS in front of RomM.
- Client-side split into FAT32/DBI parts: `0xFFFF0000` (00, 01, 02 ...) inside a temp dir. Each temp dir has a `manifest.json` with expected part sizes and which parts/partials are complete.
- Temps live under `<download_dir>/temp/<safe-12>_<id>.tmp/00.part`. After full download:
  - **Single-part**: rename/copy `00.part` to `<download_dir>/<Title or fsName>.<ext>`; temp folder and manifest removed.
  - **Multi-part**: rename `.part` -> `00/01...`, move temp to `<download_dir>/<Title or fsName>.<ext>/`, set archive bit so DBI treats it as one title; manifest removed.
- File selection: fetch `/api/roms/{id}`, pick best `.xci/.nsp` from `files[]`, build `/api/roms/{id}/content/<fs_name>?file_ids=<id>`. No hidden-folder zips.

### HUD / badges
- Shows Current and Overall progress. When all files are finalized, HUD switches to "Downloads complete".
- Badges per ROM: hollow (not queued), grey (queued), white (downloading), green (completed on disk), red (failed).
- On startup, manifests in `temp/` load as Failed (so you can retry), and final files on disk mark as Completed.
- Failures show a red "Failed: ." line. Short reads trigger a retry; if Range isn't supported that retry restarts the current ROM.
- Adding items while downloading recalculates overall bytes immediately; the overall % can dip when you enqueue mid-run (queue is not locked).

### Logging (see `logging.md`)
- Info: start/finish per ROM, finalize path, queue actions, failures.
- Debug: download heartbeats (~100?MB/10s), render traces. Enable with `log_level=debug`.

### Config knobs
- `download_dir` (default `sdmc:/romm_cache/switch`)
- `http_timeout_seconds` (default 30)
- `log_level` (`debug|info|warn|error`)

### Failure/cleanup
- Temp folders remain on failure/stop; safe to delete manually from `<download_dir>/temp/`.
- On restart, the app reuses complete parts and a single partial part using `manifest.json` (size-only validation today). If Range is unavailable, the ROM restarts from zero.
- Preflight logs HTTP status; on tiny Content-Length or 404 we refresh metadata once, then fail fast.
- Finalize logs the SD error string; single-part finalize falls back to copy-on-write if a rename fails.

### TODO (known gaps)
- Resume validation is size-only; add hashes/checks or stronger validation even though contiguity is enforced now (hashing optional/expensive; server does not provide hashes).
- Final output naming can overwrite when sanitized titles collide; add collision-safe naming (IDs or disambiguation).
- Use effective total size consistently (prefer server Content-Length when present); ensure progress/completion counters reflect that value.
- download_url from API may be relative/unencoded; absolutize against SERVER_URL and use full URL encoding for path components (spaces-only encoding is insufficient).
