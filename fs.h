/* Tiny on-disk app catalogue (table of contents) read from a fixed sector. */
#ifndef FS_H
#define FS_H

void fs_init(void);                /* read the TOC from disk */
int  fs_count(void);
const char  *fs_name(int index);
unsigned int fs_lba(int index);
unsigned int fs_sectors(int index);

/* StinkFS named files. 'name' is a NUL-padded 16-byte canonical name. */
int fs_file_write(const char *name, const void *buf, unsigned int size);
int fs_file_write_at(const char *name, const void *buf, unsigned int size,
                     unsigned int offset);
int fs_file_append(const char *name, const void *buf, unsigned int size);
int fs_file_read(const char *name, void *buf, unsigned int maxsize);
int fs_file_read_at(const char *name, void *buf, unsigned int maxsize,
                    unsigned int offset);

int fs_file_delete(const char *name);          /* remove a file, reclaim space */
int fs_file_count(void);                       /* number of files in StinkFS */
int fs_file_info(int index, char *name_out);   /* name (16B) + size, or -1 */
int fs_file_size(const char *name);            /* size in bytes, or -1 */

#endif
