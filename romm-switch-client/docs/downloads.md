# Downloads

### How it works
- One streaming HTTP GET per ROM. We stop exactly at Content-Length. If preflight sees `Accept-Ranges: bytes`, we attempt resume of partially present data; otherwise the ROM restarts on interruption.
- Client-side split into FAT32/DBI format: parts of size `0xFFFF0000` (00, 01, 02 …) inside a temp dir.
- Temps live under `<download_dir>/temp/<safe-12>.tmp/00.part`. After full download we:
  - **Single-part**: rename/copy `00.part` to `<download_dir>/<Title or fsName>.<ext>` with a copy fallback if the SD rename fails (we log strerror); then the temp folder is removed.
  - **Multi-part**: rename `.part` → `00/01…`, move the temp dir to `<download_dir>/<Title or fsName>.<ext>/`, and set the concatenation/archive bit so DBI treats it as one title.
- File selection: we always fetch `/api/roms/{id}` and pick the best `.xci/.nsp` from `files[]`, building `/api/roms/{id}/content/<fs_name>?file_ids=<id>`. We do **not** use hidden-folder zip downloads.

### HUD
- Shows Current and Overall progress. When all files are finalized, HUD switches to “Downloads complete”.
- Failures show a red “Failed: …” line. Short reads trigger a retry; if Range isn’t supported that retry restarts the current ROM.
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
- On restart, the app may reuse complete parts (if Range is available); otherwise it starts the ROM over. Overall stays aligned with current on resume.
- Finalize logs the SD error string; single-part finalize falls back to copy-on-write if a rename fails.
