## What this changes

<!-- One paragraph. What does the diff do? -->

## Why

<!-- Link the issue if there is one, or explain the motivation in a sentence or two. -->

## How I tested

<!-- Be specific. "It builds" is not testing.
     Did you `make test-headless`?
     Did you `make run` and use it?
     What did you watch for? -->

- [ ] `make` succeeds
- [ ] `make test-headless` passes
- [ ] I ran `make run` and exercised the change manually

## Touches sensitive things?

<!-- Tick any that apply — these get extra review. -->

- [ ] Syscall ABI (number, arguments, return contract)
- [ ] On-disk layout (sectors, app slots, StinkFS, TOC)
- [ ] Boot path (boot.s, GDT, paging, TSS, ELF loader)
- [ ] None of the above

## Anything reviewers should know

<!-- Tricky parts, things you're unsure about, alternatives you rejected.
     Optional but appreciated. -->
