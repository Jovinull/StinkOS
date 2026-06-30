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

void sys_exit_code(int code)
{
	__syscall(5, code, 0, 0);
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

int sys_fdelete(const char *name)
{
	return __syscall(12, (int)name, 0, 0);
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

/* ---- Additional non-inline wrappers needed by Rust callers ---- */

static inline int __syscall4_syms(int n, int a, int b, int c, int d)
{
	int ret;
	__asm__ volatile ("int $0x80"
	                  : "=a"(ret)
	                  : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d)
	                  : "memory");
	return ret;
}

/* SYS_FILLRECT (22): packed ABI ebx=(x<<16|y), ecx=(w<<16|h), edx=rgb */
void sys_fillrect(int x, int y, int w, int h, unsigned int rgb)
{
	__syscall(22, (x << 16) | (y & 0xFFFF), (w << 16) | (h & 0xFFFF), (int)rgb);
}

/* SYS_DRAWTEXT (21): ebx=x, ecx=y, edx=str, esi=rgb */
int sys_drawtext(int x, int y, const char *s, unsigned int rgb)
{
	return __syscall4_syms(21, x, y, (int)s, (int)rgb);
}

/* SYS_GETMOUSE (27): ebx=*dx, ecx=*dy, edx=*buttons */
int sys_get_mouse(int *dx, int *dy, int *buttons)
{
	return __syscall(27, (int)dx, (int)dy, (int)buttons);
}

/* SYS_EXEC (41): ebx=name */
int sys_exec(const char *name)
{
	return __syscall(41, (int)name, 0, 0);
}

/* SYS_FORK (83) */
int sys_fork(void)
{
	return __syscall(83, 0, 0, 0);
}

/* SYS_WAITPID (48): ebx=pid */
int sys_waitpid(int pid)
{
	return __syscall(48, pid, 0, 0);
}

/* SYS_SLEEP_MS (23): ebx=ms */
void sys_sleep_ms(unsigned int ms)
{
	__syscall(23, (int)ms, 0, 0);
}

/* SYS_FCOUNT (10): -> number of files in StinkFS */
int sys_fcount(void)
{
	return __syscall(10, 0, 0, 0);
}

/* SYS_FINFO (11): ebx=index ecx=name_buf(16) -> size or -1 */
int sys_finfo(int index, char *name)
{
	return __syscall(11, index, (int)name, 0);
}

/* SYS_RTC_READ (64): ebx=*sys_rtc_time -> 0 or -1 */
int sys_rtc_read(void *out)
{
	return __syscall(64, (int)out, 0, 0);
}

/* SYS_PROC_INFO (73): ebx=buf ecx=cap -> bytes written */
int sys_proc_info(char *buf, int cap)
{
	return __syscall(73, (int)buf, (int)cap, 0);
}

/* SYS_SET_KEYMAP (76): ebx=layout(0=US,1=BR) -> previous layout */
int sys_set_keymap(int layout)
{
	return __syscall(76, layout, 0, 0);
}

/* SYS_ARP_INFO (74): ebx=buf ecx=cap -> bytes written */
int sys_arp_info(char *buf, int cap)
{
	return __syscall(74, (int)buf, (int)cap, 0);
}

/* SYS_KILL (46): ebx=pid -> 0 or -1 */
int sys_kill(int pid)
{
	return __syscall(46, pid, 0, 0);
}

/* SYS_GETPID (43): -> current pid */
int sys_getpid(void)
{
	return __syscall(43, 0, 0, 0);
}

/* SYS_WIN_CREATE (85): ebx=w ecx=h -> 0 or -1 */
int sys_win_create(unsigned int w, unsigned int h)
{
	return __syscall(85, (int)w, (int)h, 0);
}

/* SYS_WIN_SHOW (86): ebx=x ecx=y edx=*title -> 0 or -1 */
int sys_win_show(int x, int y, const char *title)
{
	return __syscall(86, x, y, (int)title);
}

/* SYS_WIN_FLUSH (87) */
void sys_win_flush(void)
{
	__syscall(87, 0, 0, 0);
}

/* SYS_WIN_DESTROY (88) */
void sys_win_destroy(void)
{
	__syscall(88, 0, 0, 0);
}

/* SYS_WIN_GET_EVENT (89): ebx=*event -> 0 or -1 */
int sys_win_get_event(void *ev)
{
	return __syscall(89, (int)ev, 0, 0);
}

/* SYS_WIN_RAISE (90) */
void sys_win_raise(void)
{
	__syscall(90, 0, 0, 0);
}

/* SYS_WIN_MOVE (91): ebx=x ecx=y */
void sys_win_move(int x, int y)
{
	__syscall(91, x, y, 0);
}

/* SYS_CLIP_WRITE (92): ebx=buf ecx=len -> bytes stored */
int sys_clip_write(const void *buf, unsigned int len)
{
	return __syscall(92, (int)buf, (int)len, 0);
}

/* SYS_CLIP_READ (93): ebx=buf ecx=max -> bytes copied */
int sys_clip_read(void *buf, unsigned int max)
{
	return __syscall(93, (int)buf, (int)max, 0);
}
