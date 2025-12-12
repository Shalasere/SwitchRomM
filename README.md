# Switch Homebrew Starter

This repo now holds a canonical libnx “hello-switch” sample to verify a fresh devkitPro setup before starting the real app.

## Fresh setup (Windows / devkitPro MSYS)
1) Install devkitPro from https://devkitpro.org (choose the installer).  
2) Open the “MSYS2 / MinGW 64-bit for devkitPro” shell from the Start menu.  
3) Update/install toolchain packages:
   ```sh
   pacman -Syu
   pacman -S devkitA64 switch-dev switch-tools
   ```
4) Verify env in that shell:
   ```sh
   echo $DEVKITPRO              # expect /opt/devkitpro
   aarch64-none-elf-gcc --version
   ls $DEVKITPRO/libnx/switch_rules
   ```
   If `DEVKITPRO` is empty, run `source /etc/profile.d/devkit-env.sh` or restart the devkitPro shell.

macOS/Linux: follow the devkitPro pacman bootstrap from the docs, then install the same packages (`devkitA64 switch-dev switch-tools`) and run the same checks.

## Build and run the sample
```sh
cd hello-switch
make clean    # optional
make          # builds build/hello-switch.nro

# Optional: send over network if hbmenu is in netloader mode (press Y)
make run
```

## Deploy to SD card
Copy `hello-switch/build/hello-switch.nro` to:
- `sd:/switch/hello-switch/hello-switch.nro`
Then launch hbmenu and run it.

## Repo layout
- `hello-switch/` — minimal console “Hello, Switch” sample (libnx only).  
- `romm-switch-client/` — new SDL2-based skeleton for the RomM downloader client.  
- `.gitignore` — ignores build artifacts across subdirectories.

## RomM Switch Client (work in progress)
- Build:  
  ```sh
  cd romm-switch-client
  make
  ```
- Config expected at `sdmc:/switch/romm_switch_client/.env`, e.g.:
  ```json
  SERVER_URL=http://192.168.1.100:8080
  USERNAME=demo
  PASSWORD=demo
  PLATFORM=switch
  DOWNLOAD_DIR=sdmc:/romm_cache/switch
  API_TOKEN=
  HTTP_TIMEOUT_SECONDS=30
  FAT32_SAFE=true
  ```
- A JSON config (`config.json`) is still accepted for compatibility, but `.env` is preferred.
- Current state: SDL2 window + dummy platform/ROM lists, config parsing, basic input, placeholder download view. Networking and real downloads still to be implemented.

## Troubleshooting
- `switch_rules` not found: ensure `DEVKITPRO` is set and `$(DEVKITPRO)/libnx/switch_rules` exists; confirm you’re in the devkitPro MSYS shell.  
- TLS or PIE/link errors: make sure you’re on current devkitA64/libnx (`pacman -Syu`), and use the canonical Makefile (not the manual one).  
- `make run` fails: install `switch-tools` (`nxlink`), ensure hbmenu netloader is active and the Switch is reachable on your LAN.  
