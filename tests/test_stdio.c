/* Host test for lib/libstink_stdio.c -- the real file, with the six
 * syscalls it sits on replaced by an in-memory file.
 *
 * A buffered stream has to be indistinguishable from an unbuffered one.
 * The test keeps a reference model (a plain byte array and a position) and
 * runs a long random mix of fwrite / fread / fputc / fgetc / fseek / ftell /
 * fflush / close-and-reopen against both, comparing every byte read, every
 * position reported and the file contents whenever the stream is flushed.
 * It also checks the reason the buffer exists: many small writes must turn
 * into few write syscalls.
 */
/* No host libc headers: libstink.h declares its own printf, memcpy and
 * friends (the stream names below are renamed so that the host's stdout
 * is not replaced by ours). Everything from libstink.h is pulled in first
 * so the in-memory "kernel" can use its memcpy. */
#include "libstink.h"

/* ---- in-memory "kernel" ---- */
#define DISK_MAX (1 << 17)
static unsigned char disk[DISK_MAX];
static unsigned int  disk_len, disk_pos;
static unsigned long n_read, n_write;

static int k_open(const char *name, int flags) { (void)name; (void)flags; disk_pos = 0; return 3; }
static int k_close(int fd) { (void)fd; return 0; }
static void k_trunc(const char *name) { (void)name; disk_len = 0; }
static int k_read(int fd, void *buf, unsigned int n)
{
	(void)fd;
	n_read++;
	unsigned int left = disk_pos < disk_len ? disk_len - disk_pos : 0;
	if (n > left) n = left;
	memcpy(buf, disk + disk_pos, n);
	disk_pos += n;
	return (int)n;
}
static int k_write(int fd, const void *buf, unsigned int n)
{
	(void)fd;
	n_write++;
	if (disk_pos > disk_len || disk_pos + n > DISK_MAX) return -1;   /* no holes, no overflow */
	memcpy(disk + disk_pos, buf, n);
	disk_pos += n;
	if (disk_pos > disk_len) disk_len = disk_pos;
	return (int)n;
}
static int k_seek(int fd, int off, int whence)
{
	(void)fd;
	long base = whence == 0 ? 0 : whence == 1 ? (long)disk_pos : (long)disk_len;
	long pos = base + off;
	if (pos < 0) return -1;
	disk_pos = (unsigned int)pos;
	return (int)pos;
}

#define STDIO_SYS_READ   k_read
#define STDIO_SYS_WRITE  k_write
#define STDIO_SYS_SEEK   k_seek
#define STDIO_SYS_CLOSE  k_close
#define STDIO_SYS_OPEN   k_open
#define STDIO_SYS_TRUNC(name) k_trunc(name)
#define STDIO_BUFSZ 512u                 /* small, so the random walk crosses it constantly */

/* Our FILE objects must not replace the host libc's stdout / stderr. */
#define stdout    stk_stdout
#define stderr    stk_stderr
#define stdin     stk_stdin
#define stk_FILE  FILE
#define stk_fopen fopen
#define stk_fclose fclose
#define stk_fread fread
#define stk_fwrite fwrite
#define stk_fseek fseek
#define stk_ftell ftell
#define stk_feof  feof
#define stk_fgetc fgetc
#define stk_fputc fputc
#define stk_fflush fflush
#include "../lib/libstink_stdio.c"

/* ---- reference model ---- */
static unsigned char ref[DISK_MAX];
static unsigned int  ref_len, ref_pos;

static unsigned int rng = 0xC0FFEEu;
static unsigned int rnd(unsigned int n) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) % n; }

static int failures;
#define FAILF(...) do { if (failures++ < 10) { printf("FAIL " __VA_ARGS__); printf("\n"); } } while (0)

static void compare_disk(stk_FILE *fp, const char *when)
{
	if (stk_fflush(fp) != 0) FAILF("fflush failed %s", when);
	if (disk_len != ref_len || memcmp(disk, ref, ref_len) != 0)
		FAILF("file contents differ from the model %s (len %u vs %u)", when, disk_len, ref_len);
}

int main(void)
{
	static unsigned char chunk[9000], back[9000];
	stk_FILE *fp = stk_fopen("X", "w+");
	if (!fp) { printf("FAIL fopen\n"); return 1; }

	for (long step = 0; step < 300000 && failures == 0; step++) {
		unsigned int op = rnd(100);
		if (op < 30) {                                   /* fwrite */
			unsigned int n = rnd(10) == 0 ? rnd(9000) + 1 : rnd(40) + 1;
			if (ref_pos + n > DISK_MAX - 1) continue;
			for (unsigned int i = 0; i < n; i++) chunk[i] = (unsigned char)rnd(256);
			unsigned int w = stk_fwrite(chunk, 1, n, fp);
			if (w != n) FAILF("fwrite wrote %u of %u at step %ld", w, n, step);
			memcpy(ref + ref_pos, chunk, n);
			ref_pos += n;
			if (ref_pos > ref_len) ref_len = ref_pos;
		} else if (op < 60) {                            /* fread */
			unsigned int n = rnd(10) == 0 ? rnd(9000) + 1 : rnd(40) + 1;
			unsigned int avail = ref_len - ref_pos;
			unsigned int want = n < avail ? n : avail;
			unsigned int r = stk_fread(back, 1, n, fp);
			if (r != want) FAILF("fread returned %u, model says %u (step %ld)", r, want, step);
			else if (memcmp(back, ref + ref_pos, want) != 0) FAILF("fread data differs at step %ld", step);
			ref_pos += want;
			if (want < n && !stk_feof(fp)) FAILF("short read without EOF at step %ld", step);
		} else if (op < 70) {                            /* fputc */
			if (ref_pos + 1 > DISK_MAX - 1) continue;
			int c = (int)rnd(256);
			if (stk_fputc(c, fp) != c) FAILF("fputc failed at step %ld", step);
			ref[ref_pos++] = (unsigned char)c;
			if (ref_pos > ref_len) ref_len = ref_pos;
		} else if (op < 80) {                            /* fgetc */
			int c = stk_fgetc(fp);
			int want = ref_pos < ref_len ? ref[ref_pos] : -1;
			if (c != want) FAILF("fgetc got %d, model says %d (step %ld)", c, want, step);
			if (want >= 0) ref_pos++;
		} else if (op < 92) {                            /* fseek, never past the end */
			unsigned int target = ref_len ? rnd(ref_len + 1) : 0;
			int whence = (int)rnd(3);
			long off = whence == 0 ? (long)target
			         : whence == 1 ? (long)target - (long)ref_pos
			         : (long)target - (long)ref_len;
			if (stk_fseek(fp, off, whence) != 0) FAILF("fseek failed at step %ld", step);
			ref_pos = target;
		} else if (op < 97) {                            /* ftell */
			long at = stk_ftell(fp);
			if (at != (long)ref_pos) FAILF("ftell says %ld, model says %u (step %ld)", at, ref_pos, step);
		} else if (op < 99) {
			compare_disk(fp, "after fflush");
		} else {                                         /* close and reopen for update */
			if (stk_fclose(fp) != 0) FAILF("fclose failed at step %ld", step);
			if (disk_len != ref_len || memcmp(disk, ref, ref_len) != 0)
				FAILF("contents differ after fclose at step %ld", step);
			fp = stk_fopen("X", "r+");
			if (!fp) { FAILF("reopen failed"); break; }
			ref_pos = 0;
		}
	}
	if (fp) {
		stk_fclose(fp);
		if (disk_len != ref_len || memcmp(disk, ref, ref_len) != 0)
			FAILF("contents differ after the final fclose");
	}
	printf("%s random walk: 300000 operations, final size %u bytes\n", failures ? "FAIL" : "ok  ", ref_len);

	/* The point of the buffer: 50000 one-byte writes are not 50000 writes. */
	fp = stk_fopen("X", "w");
	n_write = 0;
	for (int i = 0; i < 50000; i++)
		stk_fputc(i & 0xFF, fp);
	stk_fclose(fp);
	unsigned long limit = 50000 / STDIO_BUFSZ + 2;
	if (n_write > limit) FAILF("50000 one-byte writes cost %lu write syscalls (limit %lu)", n_write, limit);
	else printf("ok   50000 one-byte writes cost %lu write syscalls\n", n_write);
	if (disk_len != 50000) FAILF("wrote %u bytes, want 50000", disk_len);

	/* ...and the same for reads. */
	fp = stk_fopen("X", "r");
	n_read = 0;

	for (int i = 0; i < 50000; i++) {
		int c = stk_fgetc(fp);
		if (c != (i & 0xFF)) { FAILF("byte %d read back as %d", i, c); break; }

	}
	if (stk_fgetc(fp) != -1 || !stk_feof(fp)) FAILF("no EOF after the last byte");
	stk_fclose(fp);
	if (n_read > limit + 1) FAILF("50000 one-byte reads cost %lu read syscalls", n_read);
	else printf("ok   50000 one-byte reads cost %lu read syscalls\n", n_read);

	printf("\n%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
