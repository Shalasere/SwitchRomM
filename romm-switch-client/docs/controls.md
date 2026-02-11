# Controls

Current Nintendo-style mapping (code-level, see `source/input.cpp` and on-screen hints):
- D-Pad: navigate lists
- D-Pad Left/Right (ROMS view): cycle filter/sort
- A (right): Select / confirm
- B (bottom): Back
- Y (left): Open queue
- X (top): Start downloads (from queue)
- Minus: open ROM search keyboard (ROMS view)
- R: open Diagnostics (PLATFORMS view), refresh probe (DIAGNOSTICS view)
- Plus / Start: Quit

Diagnostics launch is intentionally restricted to PLATFORMS to keep ROM footer controls uncluttered.

Mappings and UI hints are intended to stay in sync; if you change bindings in `source/input.cpp` or view gating in `source/main.cpp`, update these notes and footer hints accordingly.
