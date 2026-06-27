/* Physical memory manager: hands out 4 KiB page frames. */
#ifndef PMM_H
#define PMM_H

void pmm_init(unsigned int start, unsigned int end);
unsigned int pmm_alloc(void);          /* a 4 KiB-aligned frame, or 0 if none */
void pmm_free(unsigned int frame);

/* Introspection: total / free / used frame counts in the managed range.
 * `pmm_free_pages` includes both the watermark tail and the explicit
 * free-list pool, so it reports what `pmm_alloc` can hand out right now. */
unsigned int pmm_total_pages(void);
unsigned int pmm_free_pages(void);

#endif
