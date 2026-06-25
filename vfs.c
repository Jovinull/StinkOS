/* File-descriptor layer over StinkFS. Each open descriptor keeps the file name
 * and a byte cursor; every read/write resolves the file by name through the
 * StinkFS primitives, so descriptors stay valid even if the directory is
 * compacted by a delete elsewhere. */
#include "vfs.h"
#include "fs.h"

#define FD_MAX 16

struct fd {
	int          used;
	char         name[16];
	unsigned int off;
};

static struct fd fds[FD_MAX];

static struct fd *get(int fd)
{
	if (fd < 0 || fd >= FD_MAX || !fds[fd].used)
		return 0;
	return &fds[fd];
}

int vfs_open(const char *name, int flags)
{
	if (fs_file_size(name) < 0) {              /* does not exist yet */
		if (!(flags & O_CREATE))
			return -1;
		if (fs_file_write(name, name, 0) != 0) /* create it empty */
			return -1;
	}

	for (int i = 0; i < FD_MAX; i++) {
		if (!fds[i].used) {
			fds[i].used = 1;
			for (int k = 0; k < 16; k++)
				fds[i].name[k] = name[k];
			fds[i].off = 0;
			return i;
		}
	}
	return -1;                                 /* descriptor table full */
}

int vfs_close(int fd)
{
	struct fd *f = get(fd);
	if (!f)
		return -1;
	f->used = 0;
	return 0;
}

int vfs_read(int fd, void *buf, unsigned int n)
{
	struct fd *f = get(fd);
	if (!f)
		return -1;
	int r = fs_file_read_at(f->name, buf, n, f->off);
	if (r > 0)
		f->off += (unsigned int)r;
	return r;
}

int vfs_write(int fd, const void *buf, unsigned int n)
{
	struct fd *f = get(fd);
	if (!f)
		return -1;
	if (fs_file_write_at(f->name, buf, n, f->off) != 0)
		return -1;
	f->off += n;
	return (int)n;
}

int vfs_seek(int fd, int offset, int whence)
{
	struct fd *f = get(fd);
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
