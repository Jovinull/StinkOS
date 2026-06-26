/* File-descriptor layer over StinkFS. Each open descriptor keeps the file name
 * and a byte cursor; every read/write resolves the file by name through the
 * StinkFS primitives, so descriptors stay valid even if the directory is
 * compacted by a delete elsewhere.
 *
 * The descriptor table lives inside the per-process PCB (see proc.h). Each
 * process owns VFS_FD_MAX slots and they are isolated from peers and from
 * the kernel's boot context. If proc_current() returns NULL (extremely early
 * boot, before proc_init()), the boot_fds fallback table is used so any
 * stray vfs call still terminates instead of dereferencing NULL.
 */
#include "vfs.h"
#include "fs.h"
#include "proc.h"

static struct vfs_fd boot_fds[VFS_FD_MAX];

static struct vfs_fd *table(void)
{
	struct proc *cur = proc_current();
	return cur ? cur->fd_table : boot_fds;
}

static struct vfs_fd *get(int fd)
{
	if (fd < 0 || fd >= VFS_FD_MAX)
		return 0;
	struct vfs_fd *t = table();
	if (!t[fd].used)
		return 0;
	return &t[fd];
}

void vfs_fd_table_clear(struct vfs_fd *t)
{
	if (!t)
		return;
	for (int i = 0; i < VFS_FD_MAX; i++) {
		t[i].used = 0;
		t[i].off  = 0;
		for (int k = 0; k < 16; k++)
			t[i].name[k] = 0;
	}
}

int vfs_open(const char *name, int flags)
{
	if (fs_file_size(name) < 0) {              /* does not exist yet */
		if (!(flags & O_CREATE))
			return -1;
		if (fs_file_write(name, name, 0) != 0) /* create it empty */
			return -1;
	}

	struct vfs_fd *t = table();
	for (int i = 0; i < VFS_FD_MAX; i++) {
		if (!t[i].used) {
			t[i].used = 1;
			for (int k = 0; k < 16; k++)
				t[i].name[k] = name[k];
			t[i].off = 0;
			return i;
		}
	}
	return -1;                                 /* descriptor table full */
}

int vfs_close(int fd)
{
	struct vfs_fd *f = get(fd);
	if (!f)
		return -1;
	f->used = 0;
	return 0;
}

int vfs_read(int fd, void *buf, unsigned int n)
{
	struct vfs_fd *f = get(fd);
	if (!f)
		return -1;
	int r = fs_file_read_at(f->name, buf, n, f->off);
	if (r > 0)
		f->off += (unsigned int)r;
	return r;
}

int vfs_write(int fd, const void *buf, unsigned int n)
{
	struct vfs_fd *f = get(fd);
	if (!f)
		return -1;
	if (fs_file_write_at(f->name, buf, n, f->off) != 0)
		return -1;
	f->off += n;
	return (int)n;
}

int vfs_seek(int fd, int offset, int whence)
{
	struct vfs_fd *f = get(fd);
	if (!f)
		return -1;

	int base;
	if (whence == SEEK_SET)
		base = 0;
	else if (whence == SEEK_CUR)
		base = (int)f->off;
	else if (whence == SEEK_END) {
		base = fs_file_size(f->name);
		if (base < 0)
			return -1;
	} else {
		return -1;
	}

	int pos = base + offset;
	if (pos < 0)
		return -1;
	f->off = (unsigned int)pos;
	return pos;
}
