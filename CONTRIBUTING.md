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

## How changes land on master

There are two ways something gets onto `master`. Which one applies to you depends on your repo role.

### Direct push — admin only

The repo owner (currently `@jovinull`) can push to `master` directly:

```bash
git push origin master
```

This bypasses branch protection. CI still runs on the push, but it's **post-hoc** — it surfaces breakage after the fact, it doesn't gate the push. Used for quick fixes, small refactors, docs typos — anything where a PR is more ceremony than value.

Nobody else can do this. Force pushes and branch deletions on `master` are blocked for everyone, admins included.

### Pull request — everyone else

Every other contributor goes through a PR. That's anyone with `Write` / `Maintain` / `Triage` roles on the repo, plus anyone working from a fork. The flow:

1. **Branch from `master`.** Naming convention: `feat/...`, `fix/...`, `docs/...`, `ci/...`, `chore/...`, `refactor/...`.
2. **Commit.** Each commit lands on `master` as-is — see "Commit hygiene" below.
3. **Push and open the PR.** `gh pr create --title "feat: ..."` works fine; the GitHub web UI works fine too.
4. **CI runs `make test-headless`.** It's a **required status check** — your PR can't merge until it's green.
5. **Merge is rebase-only.** Squash and merge-commit are disabled at the repo level. Your commits land on `master` one-by-one, in order, preserved verbatim.

Admins also use PRs when they want a second pair of eyes, want to see CI run, or just feel like it.

### Why rebase-only (and what it means for you)

Because we rebase instead of squashing, **every commit you push survives on `master` permanently**. There's no "GitHub squashes my mess for me" safety net. That means:

- `git log master` shows every commit, with its original author and message.
- `git blame` points back to the actual commit that introduced the line — not to a generic "PR #14" squash commit.
- `git bisect` works at single-commit granularity, which is great when chasing regressions.

This is great when commits are clean. It's bad when they're not. Which is why commit hygiene matters more here than in a squash-based repo.

### "Squash" means two different things — don't confuse them

The word "squash" shows up in two unrelated contexts. We use one, not the other:

| Where | What it does | Status here |
|---|---|---|
| **GitHub's "Squash and merge" button** | Collapses *all* your PR commits into one commit at merge time, on the server. You don't control it. | ❌ **Disabled** at the repo level. Whatever you push is what lands on `master`. |
| **`git rebase -i`'s `squash` / `fixup` actions** | A local tool for *you* to fold your own messy commits (typo fixes, wip, fixups) into the real commits they belong to, before pushing. | ✅ **What you should use** for cleanup. |

So when this doc says "squash the fixups into the commit they belong to", it means the *local* operation. GitHub never squashes anything for you.

## Commit hygiene

Write each commit as if someone six months from now will read it alone, with no context about your PR.

- **Conventional Commits prefix.** `feat:`, `fix:`, `docs:`, `refactor:`, `chore:`, `ci:`, `test:`. Skim `git log` for the existing patterns.
- **Imperative, short subject.** `add VBE cursor blink`, not `Added VBE cursor blinking feature`.
- **Each commit should compile and tests should pass.** This is the rebase contract. If commit 5-of-10 breaks the build, `git bisect` will land there and the next person will curse you.
- **Drop the local development artifacts before pushing.** Commits like `wip`, `asdf`, `fix typo from prev commit`, `actually now it works` belong in your local history while you're working, not in `master`. Fold them into the real commit they belong to with `git rebase -i HEAD~N` (use the `fixup` or `squash` action). **To be explicit: this is not about reducing your PR to one commit.** If your work is genuinely 5 logical steps, ship 5 commits. If it's 12, ship 12. The goal is removing trash, not collapsing meaningful history.
- **Body usually unnecessary.** If you need to explain *why*, do it in the PR description. The PR sticks around as the long-form context; commits stay short.

If a PR shows up with messy commits, a maintainer will ask you to clean it up before merging. Don't take it personally — the alternative is messy commits living in `master` forever.

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

## Adding a new kernel subsystem

The codebase splits the kernel into self-contained subsystems under
`kernel/<area>/`. If you want to land a new driver or service (e.g. a new
NIC, a sensor, a filesystem), follow the existing shape so the result drops
in with zero special-cases.

1. **Pick the right area.** Drivers go under `kernel/drivers/<kind>/` (audio, input, net, storage, video, misc). Cross-cutting subsystems go under `kernel/sys/` (process table, timers, pipes). Filesystem-shaped code goes under `kernel/fs/`.

2. **Create `foo.h` + `foo.c`.** Header documents the public API only — what kernel boot code, syscalls, and other subsystems can call. Keep all state `static` in the `.c` file.

3. **Wire it into the build.**
   - Add the source basename to `C_SRCS` in the Makefile.
   - If the `.c` lives in a directory not already on `VPATH` / `KINCLUDES`, add it.
   - Add `#include "foo.h"` to `kernel/defs.h` so other translation units pick it up via the umbrella header.

4. **Initialise from `kmain` (`kernel/main.c`).** Match the existing pattern: call the init, then a single `bootdiag_add("name: brief", BOOT_OK_or_FAIL)` so the POST screen reflects the new subsystem. Place the init in dependency order (after anything it depends on, before anything that depends on it).

5. **Expose a syscall if userland needs it.** Add a numbered `case` in `kernel/sys/syscall.c`, validate every pointer via `paging_user_range_ok`, and document the new number + ABI in [`docs/SYSCALLS.md`](docs/SYSCALLS.md). Add a `sys_<name>` inline in `lib/libstink.h`.

6. **Document.** A new driver gets a one-paragraph entry in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) under "Subsystem highlights". A new on-disk layout gets a fresh `docs/<NAME>.md`. New net-stack pieces extend [`docs/NETWORK.md`](docs/NETWORK.md).

7. **Land it incrementally.** Subsystem-shaped PRs are best as a *series* of small commits: data structures first, then init + POST entry, then the first consumer (syscall or kernel caller), then the second consumer, etc. Each commit should leave the build green.

A worked example: the `kernel/sys/proc.c` PCB + scheduler landed across ~6 commits — first the PCB struct, then the process table, then the kernel-thread spawn helper, then the context-switch asm, then the PIT-driven yield, then the syscall wrappers. Each step compiled and booted on its own.

## Credit

Every contributor lands in git history — that's the real credit, and it's permanent. If you want your name in a `CONTRIBUTORS` file too, add yourself in your PR.

Thanks for being here. Now go break something.
