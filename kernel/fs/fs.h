/* StinkFS: named-file filesystem stored at fixed LBAs on disk. All app ELFs,
 * WAD assets, and persistent userland state are kept here as named files. */
#ifndef FS_H
#define FS_H

int fs_init(void);    /* load the directory from disk; 0 ok, -1 on disk error */

/* Named-file operations. 'name' is a NUL-padded 16-byte canonical name. */
int fs_file_write(const char *name, const void *buf, unsigned int size);
int fs_file_write_at(const char *name, const void *buf, unsigned int size,
                     unsigned int offset);
int fs_file_append(const char *name, const void *buf, unsigned int size);
int fs_file_read(const char *name, void *buf, unsigned int maxsize);
int fs_file_read_at(const char *name, void *buf, unsigned int maxsize,
                    unsigned int offset);

int fs_file_delete(const char *name);          /* remove a file, reclaim space */
int fs_file_count(void);                       /* number of files in StinkFS */
int fs_file_info(int index, char *name_out);   /* 16-byte name + size in bytes, or -1 */
int fs_file_size(const char *name);            /* size in bytes, or -1 */

/* Case-insensitive lookup by name: fills *lba_out and *sectors_out with the
 * absolute disk LBA and sector count of the named file. Returns 0 on success,
 * -1 if no file with that name exists. Used by the ELF loader. */
int fs_file_lba_sectors(const char *name, unsigned int *lba_out,
                        unsigned int *sectors_out);

/* Register an additional StinkFS mount at slot 'slot' (0 = A:, 1 = B:)
 * backed by ATA 'drive' (0..3), with the directory at 'dir_lba' (2
 * sectors) and data region [data_lba, data_end). Slot 0 is auto-
 * registered by fs_init at the Makefile-pinned primary location;
 * SYS_MOUNT uses this to wire additional StinkFS images at runtime.
 * Returns 0 on success, -1 if the slot is out of range / already
 * present / the directory load fails. */
int fs_mount_register(int slot, int drive, unsigned int dir_lba,
                      unsigned int data_lba, unsigned int data_end);

#endif
