# Downloads

### How it works
- One streaming HTTP GET per ROM. We stop at Content-Length. If preflight sees `Accept-Ranges: bytes`, we resume partial data (including one partial part); otherwise the ROM restarts. **HTTP only; no TLS.** Use on trusted LAN or put TLS in front of RomM.
- Client-side split into FAT32/DBI parts: `0xFFFF0000` (00, 01, 02 ...) inside a temp dir. Each temp dir has a `manifest.json` with expected part sizes and which parts/partials are complete.
- Temps live under `<download_dir>/temp/<safe-12>.tmp/00.part`. After full download:
  - **Single-part**: rename/copy `00.part` to `<download_dir>/<Title or fsName>.<ext>`; temp folder and manifest removed.
  - **Multi-part**: rename `.part` -> `00/01...`, move temp to `<download_dir>/<Title or fsName>.<ext>/`, set archive bit so DBI treats it as one title; manifest removed.
- File selection: fetch `/api/roms/{id}`, pick best `.xci/.nsp` from `files[]`, build `/api/roms/{id}/content/<fs_name>?file_ids=<id>`. No hidden-folder zips.

### HUD
- Shows Current and Overall progress. When all files are finalized, HUD switches to "Downloads complete".
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
- Finalize logs the SD error string; single-part finalize falls back to copy-on-write if a rename fails.
