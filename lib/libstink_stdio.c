/* Userland C-style stdio: a thin FILE* layer over the kernel's VFS file
 * descriptor syscalls (sys_open / sys_read / sys_write / sys_seek / sys_close).
 *
 * The FILE table is fixed-size (FILE_MAX entries). Two sentinel entries back
 * stdout and stderr -- both route their writes to sys_log() so they show up on
 * the serial debug console; stdin is not implemented (no input is ever piped
 * into a user app today, so getchar() always returns EOF). The stdio buffer
 * layer is intentionally absent: each fread/fwrite turns into one syscall, no
 * extra book-keeping for partial fills. Doom does its own buffering of WAD
 * lumps, so the extra round-trips are not a hotspot. */
#include "libstink.h"

#define FILE_MAX 16

#define FD_CLOSED    -1
#define FD_SERIAL    -2          /* writes funnel into sys_log() */

struct __file {
	int fd;
	int eof;
	int err;
	int in_use;
};

static struct __file files[FILE_MAX];

/* The two serial-bound sentinels live outside the regular table so freeing them
 * is a no-op -- fclose(stdout) silently does nothing rather than recycling a
 * table slot a future fopen could hand back. */
static struct __file serial_out = { FD_SERIAL, 0, 0, 1 };
static struct __file serial_err = { FD_SERIAL, 0, 0, 1 };

FILE *stdout = &serial_out;
FILE *stderr = &serial_err;
FILE *stdin  = (FILE *)0;

static FILE *alloc_file(int fd)
{
	for (int i = 0; i < FILE_MAX; i++) {
		if (!files[i].in_use) {
			files[i].fd     = fd;
			files[i].eof    = 0;
			files[i].err    = 0;
			files[i].in_use = 1;
			return &files[i];
		}
	}
	return (FILE *)0;
}

/* mode parsing: any combination of r/w/a with optional + (read+write) and the
 * 'b' modifier (ignored: there are no text conversions here). */
FILE *fopen(const char *name, const char *mode)
{
	if (!name || !mode)
		return (FILE *)0;

	int read = 0, write = 0, append = 0;
	for (int i = 0; mode[i] != '\0'; i++) {
		switch (mode[i]) {
		case 'r': read = 1; break;
		case 'w': write = 1; break;
		case 'a': append = 1; write = 1; break;
		case '+': read = 1; write = 1; break;
		case 'b': /* binary mode is the only mode here */ break;
		default: return (FILE *)0;
		}
	}
	if (!read && !write)
		return (FILE *)0;

	/* "w" truncates: write a zero-byte file to clear any prior contents. */
	if (write && !append && !read)
		sys_fwrite(name, "", 0);
	else if (write && !append && read)
		sys_fwrite(name, "", 0);            /* "w+" also truncates */

	int flags = write ? SYS_O_CREATE : 0;
	int fd = sys_open(name, flags);
	if (fd < 0)
		return (FILE *)0;

	if (append)
		sys_seek(fd, 0, SYS_SEEK_END);

	FILE *fp = alloc_file(fd);
	if (!fp) {
		sys_close(fd);
		return (FILE *)0;
	}
	return fp;
}

int fclose(FILE *fp)
{
	if (!fp || !fp->in_use)
		return EOF;
	if (fp->fd == FD_SERIAL) {
		/* stdout/stderr are persistent; reject the close. */
		return 0;
	}
	if (fp->fd >= 0)
		sys_close(fp->fd);
	fp->in_use = 0;
	fp->fd     = FD_CLOSED;
	return 0;
}

/* Reads up to (size * n) bytes; returns the number of full size-byte items
 * read. Sets EOF on a short read, error on a syscall failure. */
unsigned int fread(void *buf, unsigned int size, unsigned int n, FILE *fp)
{
	if (!fp || size == 0 || n == 0)
		return 0;
	if (fp->fd < 0)                                    /* closed or serial */
		return 0;

	int got = sys_read(fp->fd, buf, size * n);
	if (got < 0) {
		fp->err = 1;
		return 0;
	}
	if ((unsigned int)got < size * n)
		fp->eof = 1;
	return (unsigned int)got / size;
}

/* The serial sink wants a NUL-terminated string for sys_log(); chop the input
 * into 511-byte segments and dispatch each one. */
static unsigned int serial_write(const void *buf, unsigned int n)
{
	char chunk[512];
	const unsigned char *src = buf;
	unsigned int remaining = n;

	while (remaining > 0) {
		unsigned int take = remaining < sizeof(chunk) - 1 ? remaining : sizeof(chunk) - 1;
		for (unsigned int i = 0; i < take; i++)
			chunk[i] = (char)src[i];
		chunk[take] = '\0';
		sys_log(chunk);
		src       += take;
		remaining -= take;
	}
	return n;
}

unsigned int fwrite(const void *buf, unsigned int size, unsigned int n, FILE *fp)
{
	if (!fp || size == 0 || n == 0)
		return 0;
	if (fp->fd == FD_SERIAL) {
		serial_write(buf, size * n);
		return n;
	}
	if (fp->fd < 0)
		return 0;

	int wrote = sys_write(fp->fd, buf, size * n);
	if (wrote < 0) {
		fp->err = 1;
		return 0;
	}
	return (unsigned int)wrote / size;
}

int fseek(FILE *fp, long offset, int whence)
{
	if (!fp || fp->fd < 0)
		return -1;
	int r = sys_seek(fp->fd, (int)offset, whence);
	if (r < 0) {
		fp->err = 1;
		return -1;
	}
	fp->eof = 0;
	return 0;
}

long ftell(FILE *fp)
{
	if (!fp || fp->fd < 0)
		return -1;
	return (long)sys_seek(fp->fd, 0, SYS_SEEK_CUR);
}

void rewind(FILE *fp)
{
	if (!fp)
		return;
	fseek(fp, 0, SYS_SEEK_SET);
	fp->err = 0;
	fp->eof = 0;
}

int feof(FILE *fp)     { return fp ? fp->eof : 0; }
int ferror(FILE *fp)   { return fp ? fp->err : 0; }
void clearerr(FILE *fp){ if (fp) { fp->eof = 0; fp->err = 0; } }

int fgetc(FILE *fp)
{
	unsigned char c;
	if (fread(&c, 1, 1, fp) != 1)
		return EOF;
	return (int)c;
}

int fputc(int c, FILE *fp)
{
	unsigned char cc = (unsigned char)c;
	if (fwrite(&cc, 1, 1, fp) != 1)
		return EOF;
	return (int)cc;
}

/* Reads at most n-1 chars from fp, stopping at newline or EOF. Always NUL
 * terminates. Returns s on success, NULL if no chars were read before EOF. */
char *fgets(char *s, int n, FILE *fp)
{
	if (!s || n <= 0 || !fp)
		return (char *)0;

	int i = 0;
	while (i < n - 1) {
		int c = fgetc(fp);
		if (c == EOF) {
			if (i == 0)
				return (char *)0;
			break;
		}
		s[i++] = (char)c;
		if (c == '\n')
			break;
	}
	s[i] = '\0';
	return s;
}

int fputs(const char *s, FILE *fp)
{
	if (!s || !fp)
		return EOF;
	unsigned int len = strlen(s);
	if (fwrite(s, 1, len, fp) != len)
		return EOF;
	return 0;
}

int vfprintf(FILE *fp, const char *fmt, va_list args)
{
	char buf[1024];
	int n = vsnprintf(buf, sizeof(buf), fmt, args);
	if (n <= 0 || !fp)
		return n;

	unsigned int len = (unsigned int)n;
	if (len > sizeof(buf) - 1)                  /* vsnprintf truncated */
		len = sizeof(buf) - 1;
	fwrite(buf, 1, len, fp);
	return n;
}

int fprintf(FILE *fp, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	int n = vfprintf(fp, fmt, args);
	va_end(args);
	return n;
}

int puts(const char *s)
{
	if (!s)
		return EOF;
	if (fputs(s, stdout) == EOF)
		return EOF;
	return fputc('\n', stdout);
}

int putchar(int c)
{
	return fputc(c, stdout);
}

int getchar(void)
{
	/* No stdin yet -- no app forwards keyboard input through a FILE. */
	return EOF;
}

int fileno(FILE *fp)
{
	if (!fp || !fp->in_use)
		return -1;
	return fp->fd;                                  /* -2 for serial, real fd otherwise */
}

int isatty(int fd)
{
	(void)fd;
	return 0;                                       /* StinkOS has no controlling TTY */
}

int fflush(FILE *fp)
{
	(void)fp;
	/* Stdio here is unbuffered: every fread/fwrite turns straight into a
	 * syscall. Nothing to flush -- but the standard return value matters,
	 * so report success. */
	return 0;
}

/* sscanf: minimal Doom-compatible version. Handles "%x" (hex int) and "%i"/"%d"
 * (signed decimal int). All other format specifiers consume one argument but
 * do nothing. Returns number of items successfully matched. */
int sscanf(const char *str, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int matched = 0;
	const char *s = str;
	for (const char *f = fmt; *f != '\0'; f++) {
		if (*f != '%') { if (*s == *f) s++; continue; }
		f++;
		if (*f == '\0') break;
		if (*f == 'x' || *f == 'X') {
			unsigned int v = 0;
			while ((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F')) {
				unsigned int d;
				if (*s >= '0' && *s <= '9') d = (unsigned int)(*s - '0');
				else if (*s >= 'a' && *s <= 'f') d = (unsigned int)(*s - 'a') + 10u;
				else d = (unsigned int)(*s - 'A') + 10u;
				v = v * 16u + d;
				s++;
			}
			*va_arg(ap, unsigned int *) = v;
			matched++;
		} else if (*f == 'i' || *f == 'd') {
			int sign = 1;
			if (*s == '-') { sign = -1; s++; }
			else if (*s == '+') { s++; }
			int v = 0;
			while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
			*va_arg(ap, int *) = v * sign;
			matched++;
		} else {
			(void)va_arg(ap, void *);
		}
	}
	va_end(ap);
	return matched;
}

/* atof: parse a decimal floating-point number (no exponent). Doom uses this
 * only for config-file floats (mouse sensitivity, gamma). */
double atof(const char *s)
{
	int sign = 1;
	if (*s == '-') { sign = -1; s++; }
	else if (*s == '+') { s++; }
	double v = 0.0;
	while (*s >= '0' && *s <= '9') { v = v * 10.0 + (double)(*s - '0'); s++; }
	if (*s == '.') {
		s++;
		double frac = 0.1;
		while (*s >= '0' && *s <= '9') { v += (double)(*s - '0') * frac; frac *= 0.1; s++; }
	}
	return v * (double)sign;
}
