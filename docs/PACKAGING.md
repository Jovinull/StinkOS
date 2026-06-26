# StinkOS Packaging (.stinkpkg)

How to author a `.stinkpkg`, stand up a repository, and publish a release
that `stink-pkg` can install. The binary format is documented in
`apps/stinkpkg.h` (the C source is the source of truth); this document
covers the workflow end-to-end.

## What's in a package

A `.stinkpkg` is a self-describing binary container. Every package carries:

* `name` (up to 31 chars) -- the short, unique id used everywhere
* `version` (up to 15 chars) -- free-form string, compared as an opaque
  identifier (upgrade triggers on any inequality, not on semver order)
* `description` (up to 127 chars) -- shown in `stink-pkg query`
* a list of dependency names (other package short names; resolved
  recursively at install time)
* a file table -- for each file: name (up to 31 chars), payload offset,
  byte size
* the payload itself: file contents concatenated in table order

The wire layout, all little-endian and packed, is one fixed 196-byte
header followed by `dep_count` x 32-byte dep names, then `file_count` x
40-byte file entries, then the raw payload. See
[`apps/stinkpkg.h`](../apps/stinkpkg.h) for the exact struct
declarations -- treat that file as the spec.

## Authoring a package

`tools/make-stinkpkg.py` ships in the repo. It uses only the Python
standard library (3.x) so it runs anywhere `python3` does. Smallest
possible invocation:

```sh
tools/make-stinkpkg.py \
    --name    hello \
    --version 1.0.0 \
    --desc    "Greets you from ring 3" \
    --out     hello.stinkpkg \
    build/hello.elf
```

Multiple files:

```sh
tools/make-stinkpkg.py \
    --name editor \
    --version 0.2.1 \
    --desc "Full-screen text editor for StinkOS" \
    --out editor.stinkpkg \
    build/edit.elf  share/edit.helptxt
```

Dependencies (repeatable):

```sh
tools/make-stinkpkg.py \
    --name doom1-music \
    --version 1.0.0 \
    --desc "Doom1 music pack -- requires the doom engine" \
    --dep doom1 \
    --dep audio-extras \
    --out doom1-music.stinkpkg \
    music/*.mid
```

The tool fails closed on any oversize string (name, version, description,
file name) so the resulting file is guaranteed to parse on the kernel
side.

## Naming conventions

* Package names use lowercase and dashes (`my-app`, never `My_App`).
* File names inside the package land in StinkFS verbatim with the same
  case-insensitivity rules StinkFS uses for everything else. Pick
  uppercase 8.3-style names for executables (`DOOM1.ELF`) to match the
  existing convention; lowercase is fine for data files.
* The reserved package names `STINKDB` and `REPO_INDEX` collide with the
  on-disk state files `stink-pkg` itself maintains -- don't use them.
* The on-disk manifest `stink-pkg` writes per install is
  `pkg-<name>.lst`. Avoid file names that overlap that pattern unless
  you really want them removed when the package is uninstalled.

## Standing up a repository

A repo is just a directory of `.stinkpkg` files plus an `index.txt`
describing them. `tools/repo-server.py` is the reference implementation:
it builds the index on the fly from the files in `--pkgdir`, hashes each
one with SHA-256, and serves both routes:

```sh
mkdir -p repo
cp hello.stinkpkg editor.stinkpkg repo/
tools/repo-server.py --pkgdir repo --bind 0.0.0.0 --port 8080
```

The two routes the server exposes (and `stink-pkg` consumes):

| Route                       | Body                                                    |
|-----------------------------|---------------------------------------------------------|
| `GET /index.txt`            | One line per package: `<name> <version> <sha256>\n`     |
| `GET /pkg/<name>.stinkpkg`  | The raw archive bytes                                   |

Static hosting works too -- generate `index.txt` once with whatever
tooling you like, then drop the files behind any web server that speaks
HTTP/1.0. The kernel TCP stack does not do HTTPS yet, so the URL must be
plain `http://`.

## Telling stink-pkg where the repo lives

The default repository URL is compiled in as
`http://stinkos-repo.local`. Override it per-install by creating a
`STINKPKG.CONF` file in StinkFS with one or more `key=value` lines:

```
repo_url=http://192.168.1.50:8080
```

The cache is loaded once per `stink-pkg` invocation. Unknown keys are
ignored, so the config file is forward-compatible.

## Install flow end-to-end

```
user types 'i' in stink-pkg
   -> install_pkg(name)
        -> fetch_and_verify        (GET /pkg/name.stinkpkg + sha256 check)
        -> for each dep in header:
              install_pkg(dep, force=0)
        -> fetch_and_verify again  (dep installs clobbered pkg_buf)
        -> conflict-detection scan over pkg-*.lst
        -> unpack_package:
              for each file entry: sys_fwrite into StinkFS
              write pkg-<name>.lst manifest
              append "<name> <version>\n" to STINKDB
```

A failure at any step is fail-closed -- no partial install survives.

## Integrity

* `stink-pkg` recomputes the SHA-256 of every download and refuses any
  byte that doesn't match the value the repo index publishes.
* Hashes are 64 hex chars, lowercase, fixed-width. Anything else in the
  index gets the same refusal as a mismatch.
* An ed25519 signature **over the SHA** is a planned hardening step
  (see TODO.md §6) -- the current scheme trusts the integrity of the
  index transport, so for now keep `index.txt` and the package files on
  hosts you control.

## Removing and upgrading

```
r  remove <name>      reads pkg-<name>.lst, deletes each listed file,
                      strips the STINKDB line, frees the manifest
g  upgrade            walks STINKDB, looks each package up in REPO_INDEX,
                      re-installs any whose version string differs
```

`g` reuses the same install pipeline (sha verify, dep resolve, conflict
scan) so an upgrade is no less safe than an initial install.

## Building a sample package against the tree

To bundle one of the existing apps:

```sh
make build/edit.elf
tools/make-stinkpkg.py \
    --name   edit \
    --version 0.1.0 \
    --desc   "Stink text editor" \
    --out    edit.stinkpkg \
    build/edit.elf
mkdir -p repo && mv edit.stinkpkg repo/
tools/repo-server.py --pkgdir repo
```

From inside the running OS, point `STINKPKG.CONF` at
`http://<your-host>:8080`, then `i edit` from `stink-pkg`. The
roundtrip exercises every layer in this document.

## File-format reference

For the byte-precise wire layout (offsets, packing, every struct field),
read [`apps/stinkpkg.h`](../apps/stinkpkg.h) -- it is intentionally
self-describing and short.
