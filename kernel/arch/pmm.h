/* Physical memory manager: hands out 4 KiB page frames. */
#ifndef PMM_H
#define PMM_H

void pmm_init(unsigned int start, unsigned int end);
unsigned int pmm_alloc(void);          /* a 4 KiB-aligned frame, or 0 if none */
void pmm_free(unsigned int frame);

#endif
