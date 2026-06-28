# Credits

StinkOS is a single-author hobby OS, but several pieces of the system
exist only because someone else did the hard part first. This file
gives credit where it's due.

## Doom port

The Doom engine shipped under `apps/doom/` is **not** original to
StinkOS. It is a port of work by:

- **id Software** — original Doom source code (1993-1996), released
  under the GNU General Public License v2 in 1999. The bulk of the
  files under `apps/doom/` are descended directly from that release.
- **Simon Howard and the Chocolate Doom team** — modernised, portable
  Chocolate Doom code base that fixed thousands of compatibility and
  portability bugs in the original release while staying faithful to
  the 1993 behaviour. The `i_*` I/O abstraction model that StinkOS
  plugs into comes from there.
- **Jakub "ozkl" Świątek — [`doomgeneric`](https://github.com/ozkl/doomgeneric)**
  The thin glue that exposes Chocolate Doom's I/O surface as ~10
  plain C functions (`DG_DrawFrame`, `DG_SleepMs`, `DG_GetKey`, etc.)
  so the engine can be ported to any framebuffer / input source.
  Without doomgeneric, the StinkOS port would have meant tracing
  every SDL / X11 dependency through the Chocolate Doom tree by hand.
  The StinkOS shims (`i_*_stink.c`) implement the `DG_*` interface
  on top of `libstink` syscalls.

All three are GPL-2.0-or-later. The full licence text lives at
`apps/doom/COPYING`. See `apps/doom/README.md` for the distribution
obligation.

## Game assets

StinkOS ships the playable game using:

- **[Freedoom](https://github.com/freedoom/freedoom)** — the
  community-maintained free WADs (Freedoom 1, Freedoom 2, FreeDM)
  that act as drop-in replacements for the proprietary Ultimate Doom,
  Doom II, and Final Doom IWADs. Licensed under a permissive
  [BSD-3-Clause variant](https://github.com/freedoom/freedoom/blob/master/COPYING.adoc).
  `tools/fetch-wads.sh` downloads them; the build script bundles them
  into the StinkFS image so a fresh boot has playable Doom out of the
  box, without anyone having to bring a commercial WAD.

The original commercial `DOOM.WAD` / `DOOM2.WAD` files are **not**
redistributable. StinkOS does not include them, but a user who owns
a copy can drop their WAD onto the StinkOS filesystem and the engine
will pick it up.

## Cross-toolchain

The `tools/build-cross-toolchain.sh` script bootstraps an
`i386-elf-gcc` cross-compiler from the upstream **GCC**, **binutils**,
**GMP**, **MPC**, **MPFR** and **ISL** sources. None of that code is
vendored in this repo; the script downloads it on demand. All those
projects are licensed under their own GPL-3.0 (or compatible)
licences.

## Everything else

The kernel, libstink, the shell, the userland apps (other than Doom),
the build system, the documentation under `docs/`, the package
manager `stink-pkg`, the installer, and the CI all live under the
top-level [`LICENSE`](LICENSE) (GNU General Public License v3).
