/* Tiny on-disk app catalogue (table of contents) read from a fixed sector. */
#ifndef FS_H
#define FS_H

void fs_init(void);                /* read the TOC from disk */
int  fs_count(void);
const char  *fs_name(int index);
unsigned int fs_lba(int index);
unsigned int fs_sectors(int index);

#endif
