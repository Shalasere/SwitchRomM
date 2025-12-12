# Switch Homebrew Starter + RomM Switch Client

This repo has two targets:
- `hello-switch/`: a minimal libnx sample to verify your devkitPro toolchain.
- `romm-switch-client/`: SDL2/libnx RomM downloader client (current focus).

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
If `DEVKITPRO` is empty, `source /etc/profile.d/devkit-env.sh` or restart the devkitPro shell.

macOS/Linux: use the devkitPro pacman bootstrap, then install the same packages.

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
Place at `sdmc:/switch/romm_switch_client/.env` (sample):
```
SERVER_URL=http://your-romm-host:port   # HTTP only; TLS not supported by the client
USERNAME=demo
PASSWORD=demo
PLATFORM=switch
DOWNLOAD_DIR=sdmc:/romm_cache/switch
HTTP_TIMEOUT_SECONDS=30
FAT32_SAFE=true
LOG_LEVEL=info          # debug|info|warn|error
```
`config.json` remains supported but `.env` is preferred.

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
Mappings are fixed in `source/input.cpp` (Nintendo-style layout); UI hints match these bindings.

### Current client features
- SDL2 1280x720 UI: platforms -> ROMs -> detail, queue, downloading, error views.
- RomM API integration: fetch platforms/ROMs, then per-ROM detail fetch to pick a specific file_id (.xci/.nsp).
- Downloads: single HTTP GET per ROM, client-side FAT32/DBI splitting, resume when server supports Range, temp folder isolation, final archive-bit set on directories.
- Networking: HTTP only (no TLS); intended for trusted LAN or for deployments where HTTPS is terminated upstream of RomM.
- Logging: leveled (`LOG_LEVEL`); debug adds verbose UI/HTTP traces.
- Font: HD44780 bitmap font loaded from `romfs/HD44780_font.txt` with a custom macron glyph for O/o.

## Repo layout
- `hello-switch/` - minimal libnx sample.
- `romm-switch-client/` - full client sources (SDL, downloader, config, logging, docs in `docs/`).
- `.gitignore` - shared ignores.

## Tests (host)
- Location: `tests/`
- Build/run (requires a host C++17 compiler, not devkitPro):
  ```sh
  cd tests
  make
  ./romm_tests
  ```
Tests exercise HTTP URL parsing and chunked decoding in `api.cpp` using UNIT_TEST stubs (no Switch libs needed).

## Troubleshooting
- `switch_rules` missing or link errors: ensure `DEVKITPRO` is set and packages are current (`pacman -Syu devkitA64 switch-dev switch-tools`).
- `make run` fails: install `switch-tools` (`nxlink`), ensure hbmenu netloader is active and the Switch is reachable on LAN.
- Logging empty: check `LOG_LEVEL` in `.env` and that `sdmc:/switch/romm_switch_client/` exists and is writable.
