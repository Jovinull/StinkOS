# Contributing to StinkOS

You read the name and you're still here — already a good sign.

This is a hobby OS, not a product. The bar isn't "ships to enterprise customers." It's "makes StinkOS better, or at least more interesting." If you're new to OS dev and want to fix something small to learn, go ahead. If you're a seasoned kernel hacker who wants to add real things, also go ahead. There's room for both.

## Before you spend a weekend on something

| What you want to do | What to do first |
|---|---|
| Fix a bug | Just open a PR. Include enough info to reproduce. |
| Add a new feature | Open an issue first. Even three lines: "thinking about doing X — sound good?" Saves us both from wasted work. |
| Refactor "for cleanliness" | Probably no. The codebase is small enough that taste-driven cleanups tend to be more taste than improvement. If it actually unblocks a feature, say which one. |
| Add a new driver, port an old game, run a wild experiment | Yes please. Open an issue, drop some details, let's talk. |
| Add a dependency | No. Whole point is no external code. Talk me out of it in an issue if you really think otherwise. |

## Style

I'm not going to publish a forty-page style guide. Look at the code, match the vibe.

- **C**: K&R braces, tabs to indent, comments explain *why* when the *what* isn't obvious from the code. Don't comment well-named code.
- **Assembly**: AT&T syntax — that's what `i386-elf-as` wants.
- **Files**: lowercase, short.
- **No libraries we didn't write.** If 20 lines of C do the job, write the 20 lines. We're not building Glibc.
- **No `printf`-driven design.** If you need formatted output, add the primitive to `libstink.h` first — don't sneak in a 400-line `printf` for one feature.

## Building

You need an `i386-elf` cross-compiler and `qemu-system-i386`. See the README's "Run it" section. If you can `make run` and see the menu, you're set up.

## Testing your change

The fastest signal is:

```bash
make test-headless
```

That boots the image in QEMU, watches the serial log, and asserts the core path still works — protected mode → menu → app launch → return. If you broke any of that, you'll know in 5 seconds.

For things the headless test can't see (graphics, sound, new apps, weird key sequences), say in your PR **what you ran and what you saw**. A screenshot, a short QEMU recording, even a copy-pasted serial log — pick whatever convinces a reviewer. "It works on my machine" doesn't count.

## Pull requests

- **One logical change per PR.** Don't sneak a refactor into a bug fix.
- **Title**: short, tells me what it does. `add VGA cursor blink` beats `improvements`.
- **If your change touches the syscall ABI, the on-disk layout, or the boot path** — call it out, loud, in the description. Those are the things that bite hardest when they regress.
- **Be patient on review.** This is a side project. I might take a few days.

## What gets rejected fast

- Pulling in a dependency (anything, ever).
- Changes that "make it more like Linux" because Linux does it that way. StinkOS isn't trying to be Linux.
- PRs that work by disabling the broken thing — `if (broken) skip();` — instead of fixing the root cause.
- "Cleanup" PRs that move 40 files around without changing behavior. The diff is the cost; the value has to outweigh it.

## Filing a good issue

If you're reporting a bug, the issue template covers it. Short version:

1. What broke.
2. Smallest steps to reproduce.
3. What you expected vs. what happened.
4. Your setup (host OS, QEMU version, commit you're on).

If you're pitching a feature, tell me what it makes possible. Don't write a design doc until we've agreed it should exist.

## Credit

Every contributor lands in git history — that's the real credit, and it's permanent. If you want your name in a `CONTRIBUTORS` file too, add yourself in your PR.

Thanks for being here. Now go break something.
