/* StinkOS userland library: C wrappers around the int 0x80 syscalls, plus a
 * small set of freestanding C primitives (no libc is linked into apps) so
 * each app doesn't have to keep reinventing string/formatting helpers.
 * Syscall ABI: eax = number, ebx/ecx/edx = args, result in eax. */
#ifndef LIBSTINK_H
#define LIBSTINK_H

#include <stdarg.h>

static inline int __syscall(int n, int a, int b, int c)
{
	int ret;
	__asm__ volatile ("int $0x80"
	                  : "=a"(ret)
	                  : "a"(n), "b"(a), "c"(b), "d"(c)
	                  : "memory");
	return ret;
}

/* Four-argument variant: the fourth argument travels in esi. */
static inline int __syscall4(int n, int a, int b, int c, int d)
{
	int ret;
	__asm__ volatile ("int $0x80"
	                  : "=a"(ret)
	                  : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d)
	                  : "memory");
	return ret;
}

static inline void sys_log(const char *s)        { __syscall(1, (int)s, 0, 0); }
static inline void sys_draw(int x, int y, unsigned int rgb)
                                                 { __syscall(2, x, y, (int)rgb); }
static inline int  sys_getkey(void)              { return __syscall(3, 0, 0, 0); }
static inline unsigned int sys_alloc(void)       { return (unsigned int)__syscall(4, 0, 0, 0); }
static inline void sys_exit(void)                { __syscall(5, 0, 0, 0); }
static inline unsigned int sys_ticks(void)       { return (unsigned int)__syscall(6, 0, 0, 0); }
static inline void sys_sound(unsigned int freq)  { __syscall(7, (int)freq, 0, 0); }
static inline int  sys_fwrite(const char *name, const void *buf, unsigned int size)
                                                 { return __syscall(8, (int)name, (int)buf, (int)size); }
static inline int  sys_fread(const char *name, void *buf, unsigned int max)
                                                 { return __syscall(9, (int)name, (int)buf, (int)max); }
static inline int  sys_fcount(void)              { return __syscall(10, 0, 0, 0); }
static inline int  sys_finfo(int index, char *name)
                                                 { return __syscall(11, index, (int)name, 0); }
static inline int  sys_fdelete(const char *name) { return __syscall(12, (int)name, 0, 0); }
static inline int  sys_fappend(const char *name, const void *buf, unsigned int size)
                                                 { return __syscall(13, (int)name, (int)buf, (int)size); }
static inline int  sys_fread_at(const char *name, void *buf, unsigned int max, unsigned int off)
                                                 { return __syscall4(14, (int)name, (int)buf, (int)max, (int)off); }
static inline int  sys_fwrite_at(const char *name, const void *buf, unsigned int size, unsigned int off)
                                                 { return __syscall4(15, (int)name, (int)buf, (int)size, (int)off); }

/* File descriptors (VFS). Flags for sys_open; whence values for sys_seek. */
#define SYS_O_CREATE 1
#define SYS_SEEK_SET 0
#define SYS_SEEK_CUR 1
#define SYS_SEEK_END 2
static inline int  sys_open(const char *name, int flags)        { return __syscall(16, (int)name, flags, 0); }
static inline int  sys_close(int fd)                            { return __syscall(17, fd, 0, 0); }
static inline int  sys_read(int fd, void *buf, unsigned int n)  { return __syscall(18, fd, (int)buf, (int)n); }
static inline int  sys_write(int fd, const void *buf, unsigned int n) { return __syscall(19, fd, (int)buf, (int)n); }
static inline int  sys_seek(int fd, int offset, int whence)     { return __syscall(20, fd, offset, whence); }

/* Arrow keys have no ASCII code; the keyboard driver reports them as these
 * otherwise-unused C0 control codes, and sys_getkey() passes them through
 * unchanged. Keep in sync with KEY_* in keyboard.h on the kernel side. */
#define KEY_UP    1
#define KEY_DOWN  2
#define KEY_LEFT  3
#define KEY_RIGHT 4

/* ---- Freestanding C primitives ---- */

static inline unsigned int strlen(const char *s)
{
	unsigned int n = 0;
	while (s[n] != '\0')
		n++;
	return n;
}

static inline void *memset(void *dst, int v, unsigned int n)
{
	unsigned char *p = dst;
	for (unsigned int i = 0; i < n; i++)
		p[i] = (unsigned char)v;
	return dst;
}

static inline void *memcpy(void *dst, const void *src, unsigned int n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;
	for (unsigned int i = 0; i < n; i++)
		d[i] = s[i];
	return dst;
}

static inline int strcmp(const char *a, const char *b)
{
	while (*a != '\0' && *a == *b) {
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

static inline int strncmp(const char *a, const char *b, unsigned int n)
{
	for (unsigned int i = 0; i < n; i++) {
		if (a[i] != b[i] || a[i] == '\0')
			return (unsigned char)a[i] - (unsigned char)b[i];
	}
	return 0;
}

/* Parses a (possibly negative) decimal integer at the start of s, stopping
 * at the first non-digit. Returns 0 if there are no digits at all. */
static inline int atoi(const char *s)
{
	int sign = 1;
	int v = 0;

	if (*s == '-') {
		sign = -1;
		s++;
	} else if (*s == '+') {
		s++;
	}
	while (*s >= '0' && *s <= '9') {
		v = v * 10 + (*s - '0');
		s++;
	}
	return v * sign;
}

/* Writes the digits of v (in the given base, 2..16) into out, most
 * significant digit first, and returns how many characters were written.
 * out must have room for at least 32 digits. */
static inline int uitoa(unsigned int v, unsigned int base, char *out)
{
	char tmp[32];
	int n = 0;

	if (v == 0) {
		out[0] = '0';
		return 1;
	}
	while (v > 0 && n < 32) {
		unsigned int d = v % base;
		tmp[n++] = (d < 10) ? (char)('0' + d) : (char)('a' + d - 10);
		v /= base;
	}
	for (int i = 0; i < n; i++)
		out[i] = tmp[n - 1 - i];
	return n;
}

/* Minimal formatted logging: supports %s %c %d %u %x %%, no width/precision.
 * Renders into a fixed buffer and sends it to sys_log in one call -- exactly
 * the kind of primitive CONTRIBUTING.md asks for instead of one-off
 * formatting code sprinkled across apps. */
static inline void sys_printf(const char *fmt, ...)
{
	char buf[200];
	unsigned int p = 0;
	va_list args;

	va_start(args, fmt);
	for (const char *f = fmt; *f != '\0' && p < sizeof(buf) - 1; f++) {
		if (*f != '%') {
			buf[p++] = *f;
			continue;
		}
		f++;
		if (*f == '\0')
			break;

		char digits[32];
		int n;
		int v;

		switch (*f) {
		case 's': {
			const char *s = va_arg(args, const char *);
			while (*s != '\0' && p < sizeof(buf) - 1)
				buf[p++] = *s++;
			break;
		}
		case 'c':
			buf[p++] = (char)va_arg(args, int);
			break;
		case 'd':
			v = va_arg(args, int);
			if (v < 0) {
				buf[p++] = '-';
				v = -v;
			}
			n = uitoa((unsigned int)v, 10, digits);
			for (int i = 0; i < n && p < sizeof(buf) - 1; i++)
				buf[p++] = digits[i];
			break;
		case 'u':
			n = uitoa(va_arg(args, unsigned int), 10, digits);
			for (int i = 0; i < n && p < sizeof(buf) - 1; i++)
				buf[p++] = digits[i];
			break;
		case 'x':
			n = uitoa(va_arg(args, unsigned int), 16, digits);
			for (int i = 0; i < n && p < sizeof(buf) - 1; i++)
				buf[p++] = digits[i];
			break;
		case '%':
			buf[p++] = '%';
			break;
		default:
			buf[p++] = '%';
			if (p < sizeof(buf) - 1)
				buf[p++] = *f;
			break;
		}
	}
	va_end(args);

	buf[p] = '\0';
	sys_log(buf);
}

#endif
