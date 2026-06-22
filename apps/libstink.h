/* StinkOS userland library: C wrappers around the int 0x80 syscalls.
 * Syscall ABI: eax = number, ebx/ecx/edx = args, result in eax. */
#ifndef LIBSTINK_H
#define LIBSTINK_H

static inline int __syscall(int n, int a, int b, int c)
{
	int ret;
	__asm__ volatile ("int $0x80"
	                  : "=a"(ret)
	                  : "a"(n), "b"(a), "c"(b), "d"(c)
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

#endif
