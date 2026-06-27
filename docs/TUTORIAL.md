# Build StinkOS From Scratch

You have a Linux box (real, WSL2, or a VM) and a couple of hours. By the
end of this tutorial you will have built the i386 cross-toolchain, compiled
the kernel + userland, booted the OS under QEMU, and seen Doom run on it.
Every command here works against a clean clone -- no hidden state.

## 0. What you need

* **OS**: Linux x86_64. macOS works with Homebrew swaps (`brew install
  i686-elf-gcc qemu`) and a longer detour; Windows users either run WSL2
  or a Linux VM.
* **Disk**: ~6 GiB free for the cross-toolchain build (it spawns a
  staging tree before pruning).
* **Time**: ~30-60 minutes for the toolchain build (one-time), then ~10 s
  per incremental kernel build.
* **Brain state**: comfortable in a terminal, vaguely aware of what an
  ELF file is. No prior OS-dev experience required.

## 1. Install host dependencies

Pick the line that matches your distro.

```sh
# Debian / Ubuntu
sudo apt-get install -y \
    build-essential bison flex texinfo wget \
    libgmp3-dev libmpc-dev libmpfr-dev libisl-dev \
    qemu-system-x86 python3 cppcheck

# Fedora
sudo dnf install -y \
    gcc gcc-c++ make bison flex texinfo wget \
    gmp-devel mpc-devel mpfr-devel isl-devel \
    qemu-system-x86 python3 cppcheck

# Arch
sudo pacman -S --needed \
    base-devel bison flex texinfo wget \
    gmp libmpc mpfr isl \
    qemu-system-x86 python cppcheck
```

`cppcheck` is optional -- it powers `make audit` but the OS builds
without it.

## 2. Clone the repo

```sh
git clone https://github.com/Jovinull/StinkOS.git
cd StinkOS
```

## 3. Build the i386-elf cross-toolchain

StinkOS is built with `i386-elf-gcc`, not your host's compiler.
A helper script downloads, configures, and installs binutils + gcc under
`~/opt/cross`:

```sh
bash tools/build-cross-toolchain.sh
```

This takes 30-60 minutes on a modern laptop. The script is idempotent --
if you re-run it after a successful build, it exits early.

Once it finishes, add the toolchain to your shell path for the rest of
this session:

```sh
export PATH="$HOME/opt/cross/bin:$PATH"
i386-elf-gcc --version | head -1   # should print: i386-elf-gcc (GCC) 13.x
```

You probably want a permanent entry in your `~/.bashrc` / `~/.zshrc`.

## 4. Fetch the Freedoom WADs (optional, only if you want Doom)

```sh
bash tools/fetch-wads.sh
```

Downloads `freedoom1.wad`, `freedoom2.wad`, `freedm.wad` into `wads/`.
The build silently skips Doom slots whose WAD is missing, so you can
build a Doom-free StinkOS at any time.

## 5. Build the OS

```sh
make
```

This produces `os.bin` -- a single raw disk image containing the boot
sector, the kernel, the StinkFS directory, and every userland app. A
clean build is ~10 seconds. Incremental builds (after a kernel edit) are
sub-second.

## 6. Boot it under QEMU

```sh
make run
```

You should see the boot diagnostic panel, then the graphical menu. Pick
an app with the arrow keys + Enter. If `freedoom1.wad` was present at
build time, the DOOM1 entry will run E1M1 with sound effects.

To exit QEMU: `Ctrl-A` then `x` (or close the QEMU window).

## 7. Run the headless test

```sh
make test-headless
```

Boots the same image without a display, intercepts the serial log, and
asserts that protected mode, the timer IRQ, the keyboard IRQ, and a
handful of syscalls all worked. This is what CI runs on every push.

## 8. Try the installer

```sh
make run-install
```

Boots the image with a 128 MiB blank disk attached as drive 2. Pick the
"install" entry from the menu and confirm. The installer clones the
boot media onto the target.

Once the clone finishes, exit QEMU and run:

```sh
make run-installed
```

Now QEMU boots from the installed disk alone -- proving the install
succeeded.

## 9. Build a `.stinkpkg` and serve it

```sh
make sample-packages
tools/repo-server.py --pkgdir repo
```

Leaves `edit.stinkpkg`, `snake.stinkpkg`, etc. under `repo/` and serves
them at `http://0.0.0.0:8080`. Inside the running OS:

```
> run stink-pkg
[u] update repo index
[i] install snake
```

Walks the entire HTTP → SHA-256 → unpack → write-to-StinkFS path the
package manager exposes. The repo URL the in-OS client hits comes from
`STINKPKG.CONF` -- create it with `cat > /STINKPKG.CONF` in the shell,
content `repo_url=http://10.0.2.2:8080` (QEMU's host-loopback address).

## 10. Where to go next

* [`ARCHITECTURE.md`](ARCHITECTURE.md) -- kernel layout, paging, processes
* [`SYSCALLS.md`](SYSCALLS.md) -- every `int 0x80` number
* [`NETWORK.md`](NETWORK.md) -- TCP/IP stack walkthrough
* [`STINKFS.md`](STINKFS.md) -- the on-disk file format
* [`PACKAGING.md`](PACKAGING.md) -- author your own `.stinkpkg`
* `TODO.md` (untracked, ask for a copy) -- everything that's still pending

## Troubleshooting

| Symptom                                            | Likely cause                                            |
|----------------------------------------------------|---------------------------------------------------------|
| `i386-elf-gcc: command not found`                  | Toolchain not on PATH; redo step 3                      |
| `make` aborts on the cross-toolchain build         | Missing host dep (bison / flex / GMP); redo step 1      |
| QEMU window stays black                            | `vbe: unavailable` on serial; rebuild with `-curses`    |
| `make test-headless` hangs                         | QEMU binary missing OR firewall blocked the serial pipe |
| Doom entry greyed out                              | Run `tools/fetch-wads.sh`                               |
| Installer clones onto a too-small target           | Increase `INSTALL_TARGET_SIZE` in the Makefile          |
| `stink-pkg` says "fetch failed"                    | Check `STINKPKG.CONF`, ping the repo IP from inside OS  |

When something breaks, the first thing to check is the QEMU serial log
(`-serial stdio` adds it). The kernel logs every subsystem init, every
syscall hit, every fs write, every TCP state change -- "loud kernel,
quiet bugs" is the design rule.
