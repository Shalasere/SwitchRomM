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
PLATFORM=switch
DOWNLOAD_DIR=sdmc:/romm_cache/switch
HTTP_TIMEOUT_SECONDS=30
FAT32_SAFE=true
LOG_LEVEL=info          # debug|info|warn|error
```
`config.json` also works, but `.env` is preferred.

### Docs
- `docs/config.md` — config keys, defaults, SD paths.
- `docs/controls.md` — current controller mapping.
- `docs/downloads.md` — download pipeline and layout.
- `docs/logging.md` — logging behavior and levels.

### Controls (current mapping)
- D-Pad: navigate lists
- A (right): Back
- B (bottom): Select/confirm
- X (top): Open queue
- Y (left): Start downloads (from queue)
- Plus/Start: Quit
Mappings are fixed in `source/input.cpp` (Nintendo layout); UI hints match.

### Current client features
- SDL2 UI (1280x720): platforms -> ROMs -> detail, queue, downloading, error.
- RomM API: lists platforms/ROMs, then fetches per-ROM file_id (.xci/.nsp).
- Downloads: one HTTP GET per ROM, FAT32/DBI splits, Range resume, temp isolation, archive bit set.
- Networking: HTTP only (no TLS); run on trusted LAN or put TLS in front of RomM.
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
