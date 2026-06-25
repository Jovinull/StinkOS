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

/* Blocks the calling app for at least `ms` milliseconds. The kernel uses a
 * sti+hlt loop on the PIT (100 Hz), so the actual wakeup is rounded up to
 * the next 10 ms boundary -- accurate enough for game timing, no busy-wait. */
static inline void sys_sleep_ms(unsigned int ms)
                                                 { __syscall(23, (int)ms, 0, 0); }

/* Grow (delta > 0) or shrink (delta < 0) the program break by `delta` bytes
 * and return the old break, or (void *)-1 on failure. The break is rounded up
 * to the next 4 KiB boundary, so contiguous sbrk(N) for small N still expands
 * page-at-a-time. The userland allocator (malloc/free below) sits on top of
 * this; apps usually do not call sys_sbrk directly. */
static inline void *sys_sbrk(int delta)
{
	int r = __syscall(24, delta, 0, 0);
	return (void *)(unsigned int)r;
}

/* Userland dynamic allocator (apps/libstink_alloc.c). K&R first-fit free list
 * over sys_sbrk; coalesces adjacent free blocks on free(). The allocator has
 * file-scope state, so it must live in its own translation unit -- which is
 * why these are real prototypes (linked in) rather than static inlines. */
void *malloc(unsigned int nbytes);
void  free(void *p);
void *calloc(unsigned int n, unsigned int size);
void *realloc(void *p, unsigned int new_size);

/* Plays freq for the given number of sys_ticks(), then silences the speaker.
 * Sleeps via sys_sleep_ms (1 tick = 10 ms) instead of spinning on sys_ticks,
 * so the CPU can halt between PIT interrupts while the note plays. */
static inline void sys_tone(unsigned int freq, unsigned int ticks)
{
	sys_sound(freq);
	sys_sleep_ms(ticks * 10);
	sys_sound(0);
}
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
/* Renders a NUL-terminated string to the framebuffer at (x,y) using the
 * kernel's built-in 8x8 font (fb_text).  Needs kernel syscall 21 (SYS_DRAWTEXT):
 *   case 21: fb_text(r->ebx, r->ecx, (const char*)r->edx, r->esi); r->eax=0;
 * Returns 0 on success, -1 if the syscall is not implemented. */
static inline int  sys_drawtext(int x, int y, const char *s, unsigned int rgb)
                                                 { return __syscall4(21, x, y, (int)s, (int)rgb); }
/* Fills a rectangle with a solid colour. x,y,w,h must each fit in 16 bits
 * (true for the 1024x768 framebuffer). Packed ABI: ebx=(x<<16|y), ecx=(w<<16|h). */
static inline void sys_fillrect(int x, int y, int w, int h, unsigned int rgb)
                                                 { __syscall(22, (x << 16) | (y & 0xFFFF),
                                                              (w << 16) | (h & 0xFFFF), (int)rgb); }

/* Arrow keys have no ASCII code; the keyboard driver reports them as these
 * otherwise-unused C0 control codes, and sys_getkey() passes them through
 * unchanged. Keep in sync with KEY_* in keyboard.h on the kernel side.
 * Ctrl+letter combos come through as the standard ASCII control code
 * instead (Ctrl+A=1 ... Ctrl+Z=26), which is why these live at 28-31
 * instead of 1-4 -- that range would collide with Ctrl+A/B/C/D. */
#define KEY_UP    28
#define KEY_DOWN  29
#define KEY_LEFT  30
#define KEY_RIGHT 31

/* Standard ASCII control code for Ctrl+<letter>, e.g. KEY_CTRL('c') == 3. */
#define KEY_CTRL(c) ((c) - 'a' + 1)

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

/* Like memcpy but copes when src and dst overlap: picks the copy direction
 * based on relative order so each byte is read before its destination slot
 * gets overwritten. memcpy() above does NOT do this -- callers who can't
 * prove the regions are disjoint must reach for memmove. */
static inline void *memmove(void *dst, const void *src, unsigned int n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;
	if (d == s || n == 0)
		return dst;
	if (d < s) {
		for (unsigned int i = 0; i < n; i++)
			d[i] = s[i];
	} else {
		for (unsigned int i = n; i > 0; i--)
			d[i - 1] = s[i - 1];
	}
	return dst;
}

static inline int memcmp(const void *a, const void *b, unsigned int n)
{
	const unsigned char *pa = a;
	const unsigned char *pb = b;
	for (unsigned int i = 0; i < n; i++) {
		if (pa[i] != pb[i])
			return (int)pa[i] - (int)pb[i];
	}
	return 0;
}

static inline void *memchr(const void *p, int c, unsigned int n)
{
	const unsigned char *s = p;
	unsigned char needle = (unsigned char)c;
	for (unsigned int i = 0; i < n; i++)
		if (s[i] == needle)
			return (void *)(s + i);
	return (void *)0;
}

static inline int strcmp(const char *a, const char *b)
{
	while (*a != '\0' && *a == *b) {
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

static inline char *strcpy(char *dst, const char *src)
{
	char *d = dst;
	while ((*d++ = *src++) != '\0')
		;
	return dst;
}

/* Copies up to n characters from src into dst. If src is shorter than n the
 * remainder of dst is NUL-padded; if it is longer no terminator is written --
 * the historic POSIX behaviour callers need to be aware of. */
static inline char *strncpy(char *dst, const char *src, unsigned int n)
{
	unsigned int i;
	for (i = 0; i < n && src[i] != '\0'; i++)
		dst[i] = src[i];
	for (; i < n; i++)
		dst[i] = '\0';
	return dst;
}

static inline char *strcat(char *dst, const char *src)
{
	char *d = dst;
	while (*d != '\0')
		d++;
	while ((*d++ = *src++) != '\0')
		;
	return dst;
}

static inline char *strncat(char *dst, const char *src, unsigned int n)
{
	char *d = dst;
	while (*d != '\0')
		d++;
	for (unsigned int i = 0; i < n && src[i] != '\0'; i++)
		*d++ = src[i];
	*d = '\0';
	return dst;
}

static inline char *strchr(const char *s, int c)
{
	char needle = (char)c;
	for (; *s != '\0'; s++)
		if (*s == needle)
			return (char *)s;
	return needle == '\0' ? (char *)s : (char *)0;
}

static inline char *strrchr(const char *s, int c)
{
	char needle = (char)c;
	const char *last = (const char *)0;
	for (; *s != '\0'; s++)
		if (*s == needle)
			last = s;
	if (needle == '\0')
		return (char *)s;
	return (char *)last;
}

/* Duplicates a NUL-terminated string into a fresh malloc'd buffer, or returns
 * NULL when out of memory. Caller frees with free(). */
static inline char *strdup(const char *s)
{
	unsigned int n = strlen(s) + 1;
	char *p = malloc(n);
	if (!p)
		return (char *)0;
	memcpy(p, s, n);
	return p;
}

/* ctype-style classifiers (ASCII only, the C locale). */
static inline int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static inline int isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static inline int isupper(int c) { return c >= 'A' && c <= 'Z'; }
static inline int islower(int c) { return c >= 'a' && c <= 'z'; }
static inline int isalpha(int c) { return isupper(c) || islower(c); }
static inline int isalnum(int c) { return isalpha(c) || isdigit(c); }
static inline int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
static inline int isprint(int c) { return c >= 32 && c < 127; }
static inline int isgraph(int c) { return c > 32 && c < 127; }
static inline int ispunct(int c) { return isgraph(c) && !isalnum(c); }
static inline int toupper(int c) { return islower(c) ? c - 'a' + 'A' : c; }
static inline int tolower(int c) { return isupper(c) ? c - 'A' + 'a' : c; }

static inline int abs(int v) { return v < 0 ? -v : v; }

/* Returns pointer to first occurrence of needle in haystack, or NULL. */
static inline const char *strstr(const char *h, const char *n)
{
	if (*n == '\0')
		return h;
	for (; *h != '\0'; h++) {
		const char *p = h, *q = n;
		while (*p != '\0' && *p == *q) { p++; q++; }
		if (*q == '\0')
			return h;
	}
	return (const char *)0;
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

/* General-purpose integer parser with the C library's strtol semantics:
 *   - skips leading whitespace
 *   - optional + / -
 *   - if base == 0, auto-detects: 0x/0X = base 16, leading 0 = base 8,
 *     otherwise base 10
 *   - base 16 strips a 0x/0X prefix even when given explicitly
 *   - stops at the first character that isn't a digit in the given base
 *   - if 'end' is non-NULL, *end is set to the first unconsumed character
 * Returns 0 when no digits could be read; overflow is not signalled. */
static inline long strtol(const char *s, char **end, int base)
{
	while (isspace((unsigned char)*s))
		s++;

	int sign = 1;
	if (*s == '-') { sign = -1; s++; }
	else if (*s == '+')           s++;

	if (base == 0) {
		if (*s == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
		else if (*s == '0')                            { base = 8;  s += 1; }
		else                                             base = 10;
	} else if (base == 16 && *s == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
	}

	long val = 0;
	int  any = 0;
	for (; *s != '\0'; s++) {
		int d;
		if (*s >= '0' && *s <= '9')      d = *s - '0';
		else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
		else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
		else                              break;
		if (d >= base)
			break;
		val = val * base + d;
		any = 1;
	}

	if (end)
		*end = (char *)(any ? s : (const char *)0);   /* match glibc on no-digit */
	return sign * val;
}

/* Same parsing rules as strtol(), but no sign is consumed and the result is
 * unsigned. A leading '-' is rejected (returns 0) rather than wrapping. */
static inline unsigned long strtoul(const char *s, char **end, int base)
{
	while (isspace((unsigned char)*s))
		s++;
	if (*s == '+')
		s++;
	else if (*s == '-') {
		if (end) *end = (char *)s;
		return 0;
	}

	if (base == 0) {
		if (*s == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
		else if (*s == '0')                            { base = 8;  s += 1; }
		else                                             base = 10;
	} else if (base == 16 && *s == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
	}

	unsigned long val = 0;
	int           any = 0;
	for (; *s != '\0'; s++) {
		int d;
		if (*s >= '0' && *s <= '9')      d = *s - '0';
		else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
		else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
		else                              break;
		if (d >= base)
			break;
		val = val * (unsigned long)base + (unsigned long)d;
		any = 1;
	}

	if (end)
		*end = (char *)(any ? s : (const char *)0);
	return val;
}

static inline long atol(const char *s) { return strtol(s, (char **)0, 10); }

/* Minimal pseudo-random generator (linear congruential, same constants as
 * the classic glibc-style LCG). Good enough for game logic (food placement
 * etc.), not for anything security-sensitive. */
static unsigned int __rand_state = 1;

static inline void srand(unsigned int seed)
{
	__rand_state = seed;
}

static inline int rand(void)
{
	__rand_state = __rand_state * 1103515245u + 12345u;
	return (int)((__rand_state >> 16) & 0x7FFF);
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

/* Formatted output (apps/libstink_printf.c). Full snprintf family supporting
 * %d %i %u %x %X %o %s %c %p %% with flags ('-', '0', '#', '+', ' '), width
 * (literal or '*'), precision and the 'l'/'ll' length modifiers (treated as
 * int on this 32-bit target). No floats -- Doom is fixed-point. */
int vsnprintf(char *out, unsigned int cap, const char *fmt, va_list args);
int snprintf(char *out, unsigned int cap, const char *fmt, ...);
int vsprintf(char *out, const char *fmt, va_list args);
int sprintf(char *out, const char *fmt, ...);

/* Formatted line to the kernel debug serial. Format-side wraps snprintf, so
 * any directive snprintf supports is available here too. */
void sys_printf(const char *fmt, ...);

/* ---- C-style stdio (apps/libstink_stdio.c) ---- */

typedef struct __file FILE;

#define EOF      (-1)
#define BUFSIZ   1024

/* The same numbers as the kernel's SYS_SEEK_* and POSIX SEEK_*. Aliased here
 * so stdio code looks like portable C, even though kernel-side they pass
 * through sys_seek unchanged. */
#define SEEK_SET SYS_SEEK_SET
#define SEEK_CUR SYS_SEEK_CUR
#define SEEK_END SYS_SEEK_END

extern FILE *stdin;        /* not implemented -- always NULL */
extern FILE *stdout;       /* writes go to the kernel debug serial */
extern FILE *stderr;       /* same destination as stdout */

FILE        *fopen(const char *name, const char *mode);
int          fclose(FILE *fp);
unsigned int fread(void *buf, unsigned int size, unsigned int n, FILE *fp);
unsigned int fwrite(const void *buf, unsigned int size, unsigned int n, FILE *fp);
int          fseek(FILE *fp, long offset, int whence);
long         ftell(FILE *fp);
void         rewind(FILE *fp);
int          feof(FILE *fp);
int          ferror(FILE *fp);
void         clearerr(FILE *fp);
int          fgetc(FILE *fp);
int          fputc(int c, FILE *fp);
char        *fgets(char *s, int n, FILE *fp);
int          fputs(const char *s, FILE *fp);
int          fprintf(FILE *fp, const char *fmt, ...);
int          vfprintf(FILE *fp, const char *fmt, va_list args);
int          puts(const char *s);
int          putchar(int c);
int          getchar(void);

#endif
