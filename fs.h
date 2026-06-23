/* Tiny on-disk app catalogue (table of contents) read from a fixed sector. */
#ifndef FS_H
#define FS_H

void fs_init(void);                /* read the TOC from disk */
int  fs_count(void);
const char  *fs_name(int index);
unsigned int fs_lba(int index);
unsigned int fs_sectors(int index);

unsigned int fs_load(void);                /* read the persisted 32-bit value */
void         fs_save(unsigned int value);  /* persist a 32-bit value to disk */

/* StinkFS named files. 'name' is a NUL-padded 16-byte canonical name. */
int fs_file_write(const char *name, const void *buf, unsigned int size);
int fs_file_read(const char *name, void *buf, unsigned int maxsize);

int fs_file_count(void);                       /* number of files in StinkFS */
int fs_file_info(int index, char *name_out);   /* name (16B) + size, or -1 */

#endif
