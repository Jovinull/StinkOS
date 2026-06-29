/* Linkable (non-inline) shims for every libstink syscall wrapper.
 *
 * libstink.h declares its syscall helpers as `static inline`, which is
 * great for C call sites (zero overhead, no extra .o) but bad for any
 * caller that needs a real linker symbol -- e.g. a Rust `extern "C"`
 * import. This file emits each one as a normal function so the linker
 * can resolve the reference. C apps continue to inline at compile time
 * and never touch these symbols; --gc-sections drops them from the
 * final ELF when nothing references them.
 *
 * Note: deliberately NOT including libstink.h to avoid colliding with
 * its `static inline` definitions. The inline asm here is identical. */

static inline int __syscall(int n, int a, int b, int c)
{
	int ret;
	__asm__ volatile ("int $0x80"
	                  : "=a"(ret)
	                  : "a"(n), "b"(a), "c"(b), "d"(c)
	                  : "memory");
	return ret;
}

void sys_log(const char *s)
{
	__syscall(1, (int)s, 0, 0);
}

void sys_draw(int x, int y, unsigned int rgb)
{
	__syscall(2, x, y, (int)rgb);
}

int sys_getkey(void)
{
	return __syscall(3, 0, 0, 0);
}

unsigned int sys_alloc(void)
{
	return (unsigned int)__syscall(4, 0, 0, 0);
}

void sys_exit(void)
{
	__syscall(5, 0, 0, 0);
	__builtin_unreachable();
}

unsigned int sys_ticks(void)
{
	return (unsigned int)__syscall(6, 0, 0, 0);
}

void sys_sound(unsigned int freq)
{
	__syscall(7, (int)freq, 0, 0);
}

int sys_fwrite(const char *name, const void *buf, unsigned int size)
{
	return __syscall(8, (int)name, (int)buf, (int)size);
}

int sys_fread(const char *name, void *buf, unsigned int max)
{
	return __syscall(9, (int)name, (int)buf, (int)max);
}

/* Rust's core / alloc emits direct calls to `memcpy` / `memset` /
 * `memmove` / `memcmp` for slice ops + Vec growth + Drop. libstink.h
 * declares these `static inline`, which gives every C TU its own copy
 * but no linkable symbol -- emit non-inline ones here so Rust's
 * `extern "C"` import resolves. */
void *memset(void *dst, int v, unsigned int n)
{
	unsigned char *p = dst;
	for (unsigned int i = 0; i < n; i++)
		p[i] = (unsigned char)v;
	return dst;
}

void *memcpy(void *dst, const void *src, unsigned int n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;
	for (unsigned int i = 0; i < n; i++)
		d[i] = s[i];
	return dst;
}

void *memmove(void *dst, const void *src, unsigned int n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;
	if (d == s || n == 0)
		return dst;
	if (d < s) {
		for (unsigned int i = 0; i < n; i++) d[i] = s[i];
	} else {
		for (unsigned int i = n; i > 0; i--) d[i - 1] = s[i - 1];
	}
	return dst;
}

int memcmp(const void *a, const void *b, unsigned int n)
{
	const unsigned char *x = a, *y = b;
	for (unsigned int i = 0; i < n; i++) {
		if (x[i] != y[i]) return (int)x[i] - (int)y[i];
	}
	return 0;
}
