/* Userland C-style stdio: a FILE* layer over the kernel's VFS file
 * descriptor syscalls (sys_open / sys_read / sys_write / sys_seek / sys_close).
 *
 * The FILE table is fixed-size (FILE_MAX entries). Two sentinel entries back
 * stdout and stderr -- both route their writes to sys_log() so they show up on
 * the serial debug console; stdin is not implemented (no input is ever piped
 * into a user app today, so getchar() always returns EOF).
 *
 * Disk files are buffered, STDIO_BUFSZ bytes per FILE. It matters a great
 * deal here: a StinkFS write is a read-modify-write of a sector plus a
 * rewrite of the directory, so the unbuffered layer this replaces turned
 * Doom's byte-at-a-time save-game writer into tens of thousands of disk
 * transactions and a multi-minute freeze. A FILE is in one of three
 * states: idle, reading (buf holds bytes already fetched from the fd, the
 * fd's own position is past them) or writing (buf holds bytes not yet sent
 * to the fd). Switching direction, seeking, ftell and fclose reconcile the
 * two positions first.
 *
 * Buffered data that was written is only safe on disk after fflush() or
 * fclose(); a program that exits with a FILE still open loses it, as in C. */
#include "libstink.h"

/* The four calls the layer is built on. A host test substitutes an
 * in-memory file here (tests/test_stdio.c). */
#ifndef STDIO_SYS_READ
#define STDIO_SYS_READ   sys_read
#define STDIO_SYS_WRITE  sys_write
#define STDIO_SYS_SEEK   sys_seek
#define STDIO_SYS_CLOSE  sys_close
#define STDIO_SYS_OPEN   sys_open
#define STDIO_SYS_TRUNC(name) sys_fwrite((name), "", 0)
#endif

#define FILE_MAX 16
#ifndef STDIO_BUFSZ
#define STDIO_BUFSZ 4096u
#endif

#define FD_CLOSED    -1
#define FD_SERIAL    -2          /* writes funnel into sys_log() */

enum { BUF_IDLE = 0, BUF_READ, BUF_WRITE };

struct __file {
	int fd;
	int eof;
	int err;
	int in_use;
	unsigned char *buf;          /* STDIO_BUFSZ bytes, allocated on first use */
	unsigned int   len;          /* BUF_READ: valid bytes; BUF_WRITE: bytes pending */
	unsigned int   pos;          /* BUF_READ: next byte to hand out */
	int            mode;
};

static struct __file files[FILE_MAX];

/* The two serial-bound sentinels live outside the regular table so freeing them
 * is a no-op -- fclose(stdout) silently does nothing rather than recycling a
 * table slot a future fopen could hand back. */
static struct __file serial_out = { FD_SERIAL, 0, 0, 1, 0, 0, 0, BUF_IDLE };
static struct __file serial_err = { FD_SERIAL, 0, 0, 1, 0, 0, 0, BUF_IDLE };

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
			files[i].buf    = (unsigned char *)0;
			files[i].len    = 0;
			files[i].pos    = 0;
			files[i].mode   = BUF_IDLE;
			return &files[i];
		}
	}
	return (FILE *)0;
}

/* Send pending written bytes to the fd. Returns 0, or -1 (and sets err). */
static int flush_write(FILE *fp)
{
	if (fp->mode != BUF_WRITE)
		return 0;
	unsigned int sent = 0;
	while (sent < fp->len) {
		int w = STDIO_SYS_WRITE(fp->fd, fp->buf + sent, fp->len - sent);
		if (w <= 0) {
			fp->err = 1;
			fp->len = 0;
			fp->mode = BUF_IDLE;
			return -1;
		}
		sent += (unsigned int)w;
	}
	fp->len = 0;
	fp->mode = BUF_IDLE;
	return 0;
}

/* Forget read-ahead: move the fd back over the bytes fetched but not yet
 * handed out, so the fd position is the stream position again. */
static void drop_read(FILE *fp)
{
	if (fp->mode != BUF_READ)
		return;
	if (fp->len > fp->pos)
		STDIO_SYS_SEEK(fp->fd, -(int)(fp->len - fp->pos), SYS_SEEK_CUR);
	fp->len = fp->pos = 0;
	fp->mode = BUF_IDLE;
}

/* Bring the fd position in line with the stream position, whatever the
 * buffer was doing. */
static int sync_stream(FILE *fp)
{
	drop_read(fp);
	return flush_write(fp);
}

static int have_buffer(FILE *fp)
{
	if (!fp->buf)
		fp->buf = malloc(STDIO_BUFSZ);
	return fp->buf != (unsigned char *)0;
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
	name = stk_flat_name(name);                /* flat filesystem: last component only */

	/* "w" and "w+" truncate: write a zero-byte file to clear any prior
	 * contents. "r+" must not. */
	if (mode[0] == 'w')
		STDIO_SYS_TRUNC(name);

	int flags = write ? SYS_O_CREATE : 0;
	int fd = STDIO_SYS_OPEN(name, flags);
	if (fd < 0)
		return (FILE *)0;

	if (append)
		STDIO_SYS_SEEK(fd, 0, SYS_SEEK_END);

	FILE *fp = alloc_file(fd);
	if (!fp) {
		STDIO_SYS_CLOSE(fd);
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
	int rc = 0;
	if (fp->fd >= 0) {
		if (sync_stream(fp) != 0)
			rc = EOF;                          /* the last bytes did not reach the disk */
		STDIO_SYS_CLOSE(fp->fd);
	}
	if (fp->buf)
		free(fp->buf);
	fp->buf    = (unsigned char *)0;
	fp->in_use = 0;
	fp->fd     = FD_CLOSED;
	return rc;
}

/* Reads up to (size * n) bytes; returns the number of full size-byte items
 * read. Sets EOF when the file ends first, error on a syscall failure. */
unsigned int fread(void *buf, unsigned int size, unsigned int n, FILE *fp)
{
	if (!fp || size == 0 || n == 0)
		return 0;
	if (fp->fd < 0)                                    /* closed or serial */
		return 0;
	if (n > 0xFFFFFFFFu / size)
		return 0;                                  /* size * n does not fit */
	if (flush_write(fp) != 0)
		return 0;

	unsigned char *out = buf;
	unsigned int want = size * n, got = 0;
	while (got < want) {
		if (fp->mode == BUF_READ && fp->pos < fp->len) {
			unsigned int take = fp->len - fp->pos;
			if (take > want - got)
				take = want - got;
			for (unsigned int i = 0; i < take; i++)
				out[got + i] = fp->buf[fp->pos + i];
			fp->pos += take;
			got += take;
			continue;
		}
		fp->mode = BUF_IDLE;
		fp->len = fp->pos = 0;
		int r;
		if (want - got >= STDIO_BUFSZ || !have_buffer(fp)) {
			/* A large request (or no buffer memory): straight through. */
			r = STDIO_SYS_READ(fp->fd, out + got, want - got);
			if (r > 0)
				got += (unsigned int)r;
		} else {
			r = STDIO_SYS_READ(fp->fd, fp->buf, STDIO_BUFSZ);
			if (r > 0) {
				fp->len = (unsigned int)r;
				fp->mode = BUF_READ;
			}
		}
		if (r < 0) {
			fp->err = 1;
			break;
		}
		if (r == 0) {
			fp->eof = 1;
			break;
		}
	}
	return got / size;
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
	if (n > 0xFFFFFFFFu / size)
		return 0;
	if (fp->fd == FD_SERIAL) {
		serial_write(buf, size * n);
		return n;
	}
	if (fp->fd < 0)
		return 0;

	drop_read(fp);
	const unsigned char *in = buf;
	unsigned int want = size * n, done = 0;
	while (done < want) {
		if ((fp->mode != BUF_WRITE || fp->len == 0) &&
		    (want - done >= STDIO_BUFSZ || !have_buffer(fp))) {
			/* Nothing pending and a large block (or no buffer memory):
			 * straight through. */
			int w = STDIO_SYS_WRITE(fp->fd, in + done, want - done);
			if (w <= 0) {
				fp->err = 1;
				break;
			}
			done += (unsigned int)w;
			continue;
		}
		fp->mode = BUF_WRITE;
		unsigned int room = STDIO_BUFSZ - fp->len;
		unsigned int take = want - done < room ? want - done : room;
		for (unsigned int i = 0; i < take; i++)
			fp->buf[fp->len + i] = in[done + i];
		fp->len += take;
		done += take;
		if (fp->len == STDIO_BUFSZ && flush_write(fp) != 0)
			break;
	}
	return done / size;
}

int fseek(FILE *fp, long offset, int whence)
{
	if (!fp || fp->fd < 0)
		return -1;
	/* After this the fd position is the stream position, so SEEK_CUR
	 * means what the caller expects. */
	if (sync_stream(fp) != 0)
		return -1;
	int r = STDIO_SYS_SEEK(fp->fd, (int)offset, whence);
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
	long at = (long)STDIO_SYS_SEEK(fp->fd, 0, SYS_SEEK_CUR);
	if (at < 0)
		return -1;
	if (fp->mode == BUF_READ)
		at -= (long)(fp->len - fp->pos);           /* fetched but not handed out */
	else if (fp->mode == BUF_WRITE)
		at += (long)fp->len;                       /* accepted but not sent */
	return at;
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
	if (!fp) {                                 /* fflush(NULL): every open stream */
		int rc = 0;
		for (int i = 0; i < FILE_MAX; i++)
			if (files[i].in_use && files[i].fd >= 0 && flush_write(&files[i]) != 0)
				rc = EOF;
		return rc;
	}
	if (!fp->in_use || fp->fd < 0)
		return 0;                                  /* serial sinks are unbuffered */
	return flush_write(fp) == 0 ? 0 : EOF;
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
