# Config

 Put `.env` at `sdmc:/switch/romm_switch_client/.env`. `config.json` in the same directory is also read; current load order is `.env` then `config.json`, so JSON overrides `.env` on the same key. JSON supports schema migration (`schema_version`), and missing `schema_version` is treated as legacy schema and migrated to current keys.

## Keys (with defaults)
- `SERVER_URL` (required): Base RomM URL. **Supports `http://` and `https://`.** Example: `https://192.168.1.10:8080`.
- `USERNAME`, `PASSWORD`: Basic auth credentials. Leave empty if your server does not require them.
- `API_TOKEN`: Reserved for future token support (unused today).
- `PLATFORM` (optional): Platform slug to list; leave empty to browse/select in the UI.
- `DOWNLOAD_DIR` (`sdmc:/romm_cache`): SD base cache. Platform/ROM subfolders are created automatically; temps under `<DOWNLOAD_DIR>/temp/`.
- `HTTP_TIMEOUT_SECONDS` (`30`): HTTP send/recv timeout.
- `FAT32_SAFE` (`true`): If true, split into FAT32/DBI-sized parts (`0xFFFF0000`). If false, keep as a single file (no splitting). Multi-part handling still uses DBI archive bit when enabled.
- `LOG_LEVEL` (`info`): `debug|info|warn|error`.
- `SPEED_TEST_URL` (blank): Optional URL to fetch ~40MB (Range) for a quick throughput estimate. If set, runs once at startup; if blank, only in-download speeds are shown.

## `config.json` schema
- `schema_version` (optional, JSON only): Current supported version is `1`.
- If omitted, config is treated as legacy schema (`0`) and migrated in-memory.
- Legacy aliases accepted by migration include env-style uppercase keys (for example `SERVER_URL`) plus older aliases like `platform_id`, `download_path`, `timeout_seconds`, and `fat32_split`.

## Files created by the client
- Downloads: `<DOWNLOAD_DIR>/<Title or fsName>_<id>.<ext>` (single file, ID-suffixed for collisions) or `<DOWNLOAD_DIR>/<Title or fsName>_<id>.<ext>/00 01 ...` (multi-part DBI layout).
- Temps: `<DOWNLOAD_DIR>/temp/<safe-12>.tmp/*.part` and `manifest.json` (safe to delete after failures/stops).
- Log: `sdmc:/switch/romm_switch_client/log.txt`.
- Queue snapshot: `sdmc:/switch/romm_switch_client/queue_state.json` (pending/running queue items restored on restart; completed items are skipped).
