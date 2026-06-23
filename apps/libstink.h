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

#endif
