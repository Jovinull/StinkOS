/* Host-side test for the VFS descriptor table in kernel/fs/vfs.c.
 * Each process owns a VFS_FD_MAX-slot table; the contract:
 *   - vfs_open finds the lowest free slot, copies the name, returns
 *     its index. Table-full -> -1.
 *   - vfs_close frees the slot. Wrong fd -> -1.
 *   - vfs_read / vfs_write advance the cursor (read by return value,
 *     write by the byte count); both reject closed/oob fds.
 *   - vfs_seek with SET / CUR / END returns the new position; negative
 *     positions and unknown whence are rejected.
 *   - vfs_fd_table_clear wipes every slot.
 *
 * The disk path (fs_file_*) is stubbed so the descriptor bookkeeping
 * is tested in isolation -- that's where slot-leak and cursor-drift
 * bugs hide.
 */
#include <stdio.h>
#include <string.h>

#define VFS_FD_MAX 16

#define O_CREATE  1
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

struct vfs_fd {
	int          used;
	unsigned int off;
	char         name[16];
};

static struct vfs_fd table_[VFS_FD_MAX];

/* fs stubs. The real kernel walks StinkFS; here we keep two
 * pretend files: "exists.bin" (size 100) and nothing else. */
static int fs_file_size_stub(const char *name)
{
	if (strncmp(name, "exists.bin", 10) == 0) return 100;
	return -1;
}
static int last_write_at_offset;
static int fs_file_write_at_stub(const char *name, const void *buf,
                                 unsigned int n, unsigned int off)
{
	(void)name; (void)buf; (void)n;
	last_write_at_offset = (int)off;
	return 0;
}
static int fs_file_read_at_stub(const char *name, void *buf,
                                unsigned int n, unsigned int off)
{
	(void)name; (void)buf;
	int file_size = fs_file_size_stub(name);
	if (file_size < 0) return -1;
	if ((int)off >= file_size) return 0;
	int avail = file_size - (int)off;
	if ((int)n > avail) n = (unsigned)avail;
	return (int)n;
}
static int fs_file_write_stub(const char *name, const void *buf, unsigned int n)
{
	(void)name; (void)buf; (void)n;
	return 0;                                  /* creation always succeeds */
}

/* Mirror of vfs_open/close/read/write/seek with table_ as the table.
 * (We elide the proc_current() indirection -- single thread here.) */
static struct vfs_fd *get(int fd)
{
	if (fd < 0 || fd >= VFS_FD_MAX) return 0;
	if (!table_[fd].used) return 0;
	return &table_[fd];
}

static void table_clear(void)
{
	for (int i = 0; i < VFS_FD_MAX; i++) {
		table_[i].used = 0;
		table_[i].off  = 0;
		for (int k = 0; k < 16; k++) table_[i].name[k] = 0;
	}
}

static int vfs_open(const char *name, int flags)
{
	if (fs_file_size_stub(name) < 0) {
		if (!(flags & O_CREATE)) return -1;
		if (fs_file_write_stub(name, name, 0) != 0) return -1;
	}
	for (int i = 0; i < VFS_FD_MAX; i++) {
		if (!table_[i].used) {
			table_[i].used = 1;
			for (int k = 0; k < 16; k++) table_[i].name[k] = name[k];
			table_[i].off = 0;
			return i;
		}
	}
	return -1;
}

static int vfs_close(int fd)
{
	struct vfs_fd *f = get(fd);
	if (!f) return -1;
	f->used = 0;
	return 0;
}

static int vfs_read(int fd, void *buf, unsigned int n)
{
	struct vfs_fd *f = get(fd);
	if (!f) return -1;
	int r = fs_file_read_at_stub(f->name, buf, n, f->off);
	if (r > 0) f->off += (unsigned int)r;
	return r;
}

static int vfs_write(int fd, const void *buf, unsigned int n)
{
	struct vfs_fd *f = get(fd);
	if (!f) return -1;
	if (fs_file_write_at_stub(f->name, buf, n, f->off) != 0) return -1;
	f->off += n;
	return (int)n;
}

static int vfs_seek(int fd, int offset, int whence)
{
	struct vfs_fd *f = get(fd);
	if (!f) return -1;
	int base;
	if (whence == SEEK_SET)       base = 0;
	else if (whence == SEEK_CUR)  base = (int)f->off;
	else if (whence == SEEK_END) {
		base = fs_file_size_stub(f->name);
		if (base < 0) return -1;
	} else return -1;
	int pos = base + offset;
	if (pos < 0) return -1;
	f->off = (unsigned int)pos;
	return pos;
}

static int expect_int(const char *label, int got, int want)
{
	if (got == want) { printf("ok   %-55s = %d\n", label, got); return 0; }
	printf("FAIL %s: got %d, want %d\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;
	int fd, fd2;
	char buf[16];

	/* --- existing file: open returns slot 0, no create flag needed. */
	table_clear();
	fd = vfs_open("exists.bin", 0);
	failures += expect_int("open existing -> 0", fd, 0);
	failures += expect_int("slot 0 used after open", table_[0].used, 1);

	/* --- nonexistent without O_CREATE -> -1 ------------------ */
	fd2 = vfs_open("new.bin", 0);
	failures += expect_int("open missing no O_CREATE -> -1", fd2, -1);

	/* --- second open: gets slot 1 ---------------------------- */
	fd2 = vfs_open("exists.bin", 0);
	failures += expect_int("second open -> 1", fd2, 1);

	/* --- close first, third open re-uses slot 0 ------------- */
	failures += expect_int("close fd=0 -> 0", vfs_close(fd), 0);
	int fd3 = vfs_open("exists.bin", 0);
	failures += expect_int("re-open after close -> 0 (lowest free)",
	                       fd3, 0);

	/* --- close of unused / oob fd ------------------------- */
	failures += expect_int("close already-closed slot 5 -> -1",
	                       vfs_close(5), -1);
	failures += expect_int("close oob fd -1 -> -1", vfs_close(-1), -1);
	failures += expect_int("close oob fd MAX -> -1", vfs_close(VFS_FD_MAX), -1);

	/* --- table exhaustion --------------------------------- */
	table_clear();
	for (int i = 0; i < VFS_FD_MAX; i++) {
		failures += expect_int("fill table", vfs_open("exists.bin", 0), i);
	}
	failures += expect_int("table full -> -1",
	                       vfs_open("exists.bin", 0), -1);

	/* --- read advances cursor by return value ------------- */
	table_clear();
	fd = vfs_open("exists.bin", 0);                      /* file size 100 */
	failures += expect_int("read 40 -> 40", vfs_read(fd, buf, 40), 40);
	failures += expect_int("cursor at 40", (int)table_[fd].off, 40);
	failures += expect_int("read 30 -> 30", vfs_read(fd, buf, 30), 30);
	failures += expect_int("cursor at 70", (int)table_[fd].off, 70);
	failures += expect_int("read 40 (only 30 left) -> 30",
	                       vfs_read(fd, buf, 40), 30);
	failures += expect_int("cursor at end 100", (int)table_[fd].off, 100);
	failures += expect_int("read at EOF -> 0",
	                       vfs_read(fd, buf, 10), 0);

	/* --- write advances cursor by n unconditionally ------- */
	table_clear();
	fd = vfs_open("exists.bin", 0);
	failures += expect_int("write 25 -> 25", vfs_write(fd, "x", 25), 25);
	failures += expect_int("cursor 25 after write",
	                       (int)table_[fd].off, 25);
	failures += expect_int("write_at offset was 0",
	                       last_write_at_offset, 0);
	failures += expect_int("write 30 -> 30", vfs_write(fd, "x", 30), 30);
	failures += expect_int("cursor 55 after write", (int)table_[fd].off, 55);
	failures += expect_int("write_at offset was 25",
	                       last_write_at_offset, 25);

	/* --- seek SET / CUR / END ----------------------------- */
	table_clear();
	fd = vfs_open("exists.bin", 0);
	failures += expect_int("seek SET 40 -> 40",
	                       vfs_seek(fd, 40, SEEK_SET), 40);
	failures += expect_int("cursor 40 after seek SET",
	                       (int)table_[fd].off, 40);
	failures += expect_int("seek CUR +10 -> 50",
	                       vfs_seek(fd, 10, SEEK_CUR), 50);
	failures += expect_int("seek CUR -5 -> 45",
	                       vfs_seek(fd, -5, SEEK_CUR), 45);
	failures += expect_int("seek END 0 -> 100",
	                       vfs_seek(fd, 0, SEEK_END), 100);
	failures += expect_int("seek END -20 -> 80",
	                       vfs_seek(fd, -20, SEEK_END), 80);
	failures += expect_int("seek SET -1 -> reject (-1)",
	                       vfs_seek(fd, -1, SEEK_SET), -1);
	failures += expect_int("seek SET 0 -> 0",
	                       vfs_seek(fd, 0, SEEK_SET), 0);
	failures += expect_int("seek unknown whence -> -1",
	                       vfs_seek(fd, 0, 99), -1);

	/* --- ops on closed fd reject ----------------------- */
	(void)vfs_close(fd);
	failures += expect_int("read closed -> -1",  vfs_read(fd, buf, 1), -1);
	failures += expect_int("write closed -> -1", vfs_write(fd, "x", 1), -1);
	failures += expect_int("seek closed -> -1",  vfs_seek(fd, 0, SEEK_SET), -1);

	/* --- fd table clear wipes everything ------------- */
	table_clear();
	fd = vfs_open("exists.bin", 0);
	vfs_write(fd, "x", 50);
	table_clear();
	failures += expect_int("after clear: slot 0 unused", table_[0].used, 0);
	failures += expect_int("after clear: slot 0 off=0", (int)table_[0].off, 0);
	failures += expect_int("after clear: read of fd 0 -> -1",
	                       vfs_read(0, buf, 1), -1);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
