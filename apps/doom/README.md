# Doom port for StinkOS

This directory contains the Doom engine source code that ships as a
StinkOS userland app. The code is **not original** to StinkOS — it is
a port of:

- **Linux Doom** by id Software (1993-1996)
- **Chocolate Doom** by Simon Howard et al. (2005-2014)
- **doomgeneric** by Jakub "ozkl" Świątek — a thin abstraction layer
  that exposes Chocolate Doom's I/O surface as ~10 plain C functions
  (`DG_DrawFrame`, `DG_SleepMs`, `DG_GetKey`, etc.), making it trivial
  to port Doom to any framebuffer / input source

The StinkOS-specific shim lives in `i_*_stink.c` files; everything
else is upstream code.

## Licence

All code in this directory is licensed under the **GNU General Public
License v2.0 or later**. The full text is in [`COPYING`](COPYING).

This is a different licence from the parent StinkOS project, which is
GPL-3.0. The two are compatible (any GPL-2.0-or-later code can be
combined with GPL-3.0 code, and the result is GPL-3.0). The reason
this folder keeps its own `COPYING` is that the GPL requires the
licence text to ship alongside the code it covers — we keep it
local to `apps/doom/` so the repo root only advertises StinkOS's own
GPL-3.0 `LICENSE`.

## Distribution obligation

If you ship a StinkOS image that includes the Doom binary, you must
also make the source code available to whoever you ship to. The
source is this directory plus the StinkOS kernel + libstink it links
against. Pointing them at the StinkOS git repo satisfies that.

## WAD assets

This directory does **not** include the playable WAD files. Doom-the-
game needs an IWAD (`doom1.wad`, `doom2.wad`, etc.). StinkOS bundles
the freely-redistributable Freedoom replacements (Freedoom 1, Freedoom
2, FreeDM) under a separate permissive licence — see the top-level
README for details and `tools/fetch-wads.sh` for the download script.
The original commercial id Software WADs are **not** redistributable;
if you want to play Ultimate Doom or Doom 2 with the real assets, you
have to bring your own WAD.
