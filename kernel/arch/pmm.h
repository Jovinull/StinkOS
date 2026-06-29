/* Physical memory manager: hands out 4 KiB page frames. */
#ifndef PMM_H
#define PMM_H

void pmm_init(unsigned int start, unsigned int end);
unsigned int pmm_alloc(void);          /* a 4 KiB-aligned frame, or 0 if none */

/* Decrement the frame's refcount; the frame is returned to the free pool
 * only when the refcount hits zero. Without prior pmm_ref_inc calls a
 * fresh `pmm_alloc(); pmm_free(same)` pair behaves exactly as before. */
void pmm_free(unsigned int frame);

/* Bump the refcount on an already-allocated frame so multiple owners
 * (e.g. parent + child after COW fork) can hold it. Each `pmm_ref_inc`
 * must be paired with a matching `pmm_free` from the same owner so the
 * frame actually returns to the pool. */
void pmm_ref_inc(unsigned int frame);

/* Read the current refcount. 0 = not allocated; 1 = single owner; >1 =
 * shared (typically a COW page pre-write). Diagnostic only -- callers
 * should not branch on this value without holding a lock once SMP lands. */
unsigned int pmm_ref(unsigned int frame);

/* Introspection: total / free / used frame counts in the managed range.
 * `pmm_free_pages` includes both the watermark tail and the explicit
 * free-list pool, so it reports what `pmm_alloc` can hand out right now. */
unsigned int pmm_total_pages(void);
unsigned int pmm_free_pages(void);

#endif
