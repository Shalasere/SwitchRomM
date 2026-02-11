# Switch Homebrew Starter + RomM Switch Client

Two things here:
- `hello-switch/`: minimal libnx sample to check your devkitPro setup.
- `romm-switch-client/`: SDL2/libnx RomM downloader (the one you want).

## Toolchain (Windows / devkitPro MSYS)
1) Install devkitPro from https://devkitpro.org.  
2) Open the "MSYS2 / MinGW 64-bit for devkitPro" shell.  
3) Install/update packages:
   ```sh
   pacman -Syu
   pacman -S devkitA64 switch-dev switch-tools
   ```
4) Verify:
   ```sh
   echo $DEVKITPRO              # expect /opt/devkitpro
   aarch64-none-elf-gcc --version
   ls $DEVKITPRO/libnx/switch_rules
   ```
If `DEVKITPRO` is empty, `source /etc/profile.d/devkit-env.sh` or restart the devkitPro shell. macOS/Linux: use the devkitPro pacman bootstrap, then install the same packages.

## Build and run (hello-switch)
```sh
cd hello-switch
make clean   # optional
make         # build/hello-switch.nro
make run     # sends via nxlink if hbmenu netloader (Y) is active
```
Manual deploy: copy `build/hello-switch.nro` to `sd:/switch/hello-switch/hello-switch.nro`.

## Build and run (romm-switch-client)
```sh
cd romm-switch-client
make clean && make        # produces romm-switch-client.nro
make run                  # nxlink to a Switch in netloader mode
```

### Runtime config (.env)
Put `.env` at `sdmc:/switch/romm_switch_client/.env` (sample):
```
SERVER_URL=http://YOUR_ROMM_HOST:PORT      # HTTP only; TLS not supported by the client
USERNAME=your_username
PASSWORD=your_password
DOWNLOAD_DIR=sdmc:/romm_cache               # base cache; platform/title_id subfolders are created
HTTP_TIMEOUT_SECONDS=30
FAT32_SAFE=true
LOG_LEVEL=info          # debug|info|warn|error
SPEED_TEST_URL=         # optional; URL to fetch ~40MB (Range) to estimate throughput; leave blank to skip
```
`config.json` is also read and currently overrides `.env` on the same keys (load order is .env then config.json).

### Docs
- `docs/config.md` - config keys, defaults, SD paths.
- `docs/controls.md` - current controller mapping.
- `docs/downloads.md` - download pipeline, resume/retry rules, badges.
- `docs/logging.md` - logging behavior and levels.

### Controls (current mapping)
- D-Pad: navigate lists
- B (bottom): Back
- A (right): Select / confirm
- Y (left): Queue view / add to queue
- X (top): Start downloads (from queue)
- D-Pad Left/Right (ROMS view): cycle filter/sort
- Minus (ROMS view): text search
- R (PLATFORMS view): open diagnostics
- R (DIAGNOSTICS view): refresh reachability probe
- Plus/Start: Quit
Mappings are fixed in `source/input.cpp` (positional codes); UI hints match.

### Queue / status behavior
- Badges per ROM: hollow (not queued), grey (queued), white (downloading), green (completed on disk), orange (resumable), red (failed).
- Footer shows status text for the selected ROM.
- Completed detection checks final output on disk under `<download_dir>/<platform>/<title_id>/...` (flat fallbacks supported).
- No duplicate enqueue per session; failed/incomplete items can be retried, completed are blocked. Resumable items show as orange and can be retried manually; they are not auto-queued.
- Temp manifests load into history as Resumable ("Resume available") and do not auto-queue; 404/tiny preflight triggers one metadata refresh, then fails fast.

### Current client features
- SDL2 UI (1280x720): platforms -> ROMs -> detail, queue, downloading, diagnostics, error.
- RomM API: lists platforms/ROMs, fetches per-ROM files[]; bundles respect relative paths; per-ROM folder naming `title_id`.
- ROM list tooling: revision-keyed in-memory index for search/filter/sort without full per-frame scans.
- Diagnostics screen: config summary, server reachability probe, SD free space, queue/history stats, last error, and exportable log summary.
- Downloads: FAT32/DBI splits when enabled, Range resume with contiguity enforcement, temp isolation under `<download_dir>/temp/<platform>/<rom>/<file>/...`, archive bit set for multi-part.
- Networking: HTTP only (no TLS); run on trusted LAN or put TLS in front of RomM. Redirects are not followed (Location logged).
- Logging: leveled (`LOG_LEVEL`); debug is noisy.
- Font: HD44780 bitmap font from `romfs/HD44780_font.txt` with macron glyph.

## Repo layout
- `hello-switch/` - minimal libnx sample.
- `romm-switch-client/` - full client sources (SDL, downloader, config, logging, docs in `docs/`).
- `.gitignore` - shared ignores.

## Tests (host)
- Location: `tests/`
- Build/run (host C++17 compiler + `make`, not devkitPro):
  ```sh
  cd tests
  make                 # uses host g++/make; run from MSYS2 MinGW64 on Windows
  ./romm_tests         # Catch2 runner; use -s or --list-tests for detail
  ```
Tests cover HTTP-only URL parsing (defaults + rejects https) and strict chunked decoding (valid/malformed, extensions, missing CRLF). No Switch libs needed.
- Windows (MSYS2/MinGW64): install host tools if missing:
  ```
  pacman -S gcc make
  ```
Use the **MSYS2 MinGW64** shell (not PowerShell/MSYS). Override compiler if needed: `CXX=/usr/bin/g++ make`.

## Troubleshooting
- `switch_rules` missing or link errors: ensure `DEVKITPRO` is set and packages are current (`pacman -Syu devkitA64 switch-dev switch-tools`).
- `make run` fails: install `switch-tools` (`nxlink`), ensure hbmenu netloader is active and the Switch is reachable on LAN.
- Logging empty: check `LOG_LEVEL` in `.env` and that `sdmc:/switch/romm_switch_client/` exists and is writable.
