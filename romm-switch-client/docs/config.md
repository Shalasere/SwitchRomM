# Config

Put `.env` at `sdmc:/switch/romm_switch_client/.env`. `config.json` in the same directory is also read, but `.env` wins if both set the same key.

## Keys (with defaults)
- `SERVER_URL` (required): Base RomM URL. **HTTP only; TLS is not supported in the client.** Example: `http://192.168.1.10:8080`.
- `USERNAME`, `PASSWORD`: Basic auth credentials. Leave empty if your server does not require them.
- `API_TOKEN`: Reserved for future token support (unused today).
- `PLATFORM` (`switch`): Platform slug to list.
- `DOWNLOAD_DIR` (`sdmc:/romm_cache/switch`): SD destination. Temps under `<DOWNLOAD_DIR>/temp/`.
- `HTTP_TIMEOUT_SECONDS` (`30`): HTTP send/recv timeout.
- `FAT32_SAFE` (`true`): Present but ignored; splitting is always on.
- `LOG_LEVEL` (`info`): `debug|info|warn|error`.

## Files created by the client
- Downloads: `<DOWNLOAD_DIR>/<Title or fsName>` (single file) or `<DOWNLOAD_DIR>/<Title or fsName>/00 01 ...` (multi-part DBI layout).
- Temps: `<DOWNLOAD_DIR>/temp/<safe-12>.tmp/*.part` and `manifest.json` (safe to delete after failures/stops).
- Log: `sdmc:/switch/romm_switch_client/log.txt`.
