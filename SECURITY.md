# Security Policy

Let's keep expectations real: this is a hobby x86 operating system that boots in QEMU. If you "exploit" it, you've probably found a logic bug, not something that's going to ransomware Wall Street.

That said — bugs are bugs, and ones with a security flavor deserve thoughtful handling. Here's how to report them and what I'll do with them.

## What counts as a security issue here

The security model of StinkOS is small but real:

> A userland app cannot take down the kernel, read kernel memory, or read another app's memory. The kernel is the boundary, syscalls are the only doorway, and `int 0x80` argument validation is what enforces it.

Anything that breaks that model is a security issue. Concretely:

- A user app can crash, escalate, or read kernel memory **through the syscall surface** — by passing arguments that should be rejected but aren't being validated.
- The bootloader or kernel mishandles untrusted input from disk (an ELF binary, a StinkFS entry) in a way that bypasses isolation or executes attacker-controlled code with kernel privilege.
- A bug in the ELF loader, page table setup, or TSS that breaks the Ring 3 / Ring 0 boundary.
- A bug in StinkFS that lets one app corrupt another's files.

Basically: anything where the line above stops being true.

## What's not a security issue

- "The keyboard driver only handles ASCII." That's a feature gap. Open a normal issue.
- "If I write garbage over my own program's pages, my program crashes." That's the OS doing its job.
- "I rebuilt the image with a malicious app in slot 1 and it ran." You replaced the disk image. Yes — at that point the whole machine is the attacker.
- "On real hardware, the kernel runs as Ring 0 and can do anything." Correct. That's how operating systems work.

## How to report

**Please don't open a public issue for security bugs.**

Email **felipejovinogamerplay@gmail.com** with:

- **What you found.** One sentence is fine.
- **How to reproduce it.** Ideally a tiny app, a modified disk image, or exact steps.
- **What you think the impact is.** Even a guess helps me prioritize.

I'll acknowledge within 7 days. If the bug is real, I'll fix it, credit you in the commit (or anonymously if you prefer), and write a short note in the release if it matters.

## Disclosure timeline

Give me **30 days** to fix before going public. After that, write about it however you want.

30 days is long for a one-person hobby OS, I know. The reason isn't that I think it's critical — it's that I have a day job and weekends fill up fast. If something needs faster turnaround (you found it because you're using StinkOS for something real, somehow), say so and I'll prioritize.

## What I won't do

- Pay a bug bounty. There isn't one. I'm not running an HR department for vulnerabilities.
- Sign an NDA. Reports are private until they're public.
- Argue about whether your finding "really" counts as security. If you took the time to report it carefully, I'll take the time to handle it carefully — even if it ends up being a regular bug.

Thanks for not being the person who drops a 0-day on Twitter before saying hi.
