/* Virtual file layer: Unix-like file descriptors over StinkFS. A descriptor
 * remembers a file by name and a byte cursor, so reads and writes advance
 * sequentially and seeks reposition the cursor. Each process owns its own
 * descriptor table embedded in the PCB (see kernel/sys/proc.h). */
#ifndef VFS_H
#define VFS_H

#define VFS_FD_MAX     16          /* per-process descriptor table size */
#define O_CREATE 1                 /* create the file if it does not exist */

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* One open descriptor. Embedded in struct proc as fd_table[VFS_FD_MAX]; not
 * intended for use outside the vfs / proc layer. */
struct vfs_fd {
	int          used;
	char         name[16];
	unsigned int off;
};

/* Reset a per-process descriptor table (called when allocating a fresh PCB). */
void vfs_fd_table_clear(struct vfs_fd *table);

/* 'name' is a NUL-padded 16-byte canonical name (see the syscall layer). */
int vfs_open(const char *name, int flags);     /* -> fd, or -1 */
int vfs_close(int fd);
int vfs_read(int fd, void *buf, unsigned int n);        /* -> bytes, or -1 */
int vfs_write(int fd, const void *buf, unsigned int n); /* -> bytes, or -1 */
int vfs_seek(int fd, int offset, int whence);           /* -> new cursor, or -1 */

#endif
