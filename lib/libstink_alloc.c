/* Userland dynamic allocator, layered on the SYS_SBRK syscall. The design is
 * the classic K&R first-fit free list (Section 8.7): blocks are sized in units
 * of one header, the free list is kept circular and sorted by address, and
 * adjacent free blocks are coalesced on every free. Big enough for Doom and
 * the general "lots of small malloc/free" workloads, small enough to read in
 * one sitting. */
#include "libstink.h"

typedef double align_t;

union header {
	struct {
		union header *next;
		unsigned int  size;        /* size in units of one header */
	} s;
	align_t pad;                       /* forces 8-byte block alignment */
};

typedef union header Header;

/* The sentinel block: a permanently-empty node that anchors the circular free
 * list. Every traversal starts here, so freep never needs to be NULL after the
 * first malloc/free. */
static Header  base;
static Header *freep = (Header *)0;

/* Minimum number of header-units to request from the kernel per sbrk(). Larger
 * values trade a bit of resident memory for many fewer syscalls. */
#define NALLOC 1024u

static Header *morecore(unsigned int nunits)
{
	if (nunits < NALLOC)
		nunits = NALLOC;

	void *cp = sys_sbrk((int)(nunits * sizeof(Header)));
	if (cp == (void *)(unsigned int)-1)
		return (Header *)0;

	Header *up = (Header *)cp;
	up->s.size = nunits;
	free((void *)(up + 1));            /* drop the fresh chunk on the free list */
	return freep;
}

void *malloc(unsigned int nbytes)
{
	unsigned int nunits = (nbytes + sizeof(Header) - 1) / sizeof(Header) + 1;

	if (freep == (Header *)0) {
		base.s.next = freep = &base;
		base.s.size = 0;
	}

	Header *prevp = freep;
	for (Header *p = prevp->s.next; ; prevp = p, p = p->s.next) {
		if (p->s.size >= nunits) {
			if (p->s.size == nunits) {                /* exact fit: unlink */
				prevp->s.next = p->s.next;
			} else {                                  /* split off the tail */
				p->s.size -= nunits;
				p += p->s.size;
				p->s.size = nunits;
			}
			freep = prevp;
			return (void *)(p + 1);
		}
		if (p == freep) {                                 /* wrapped the list */
			p = morecore(nunits);
			if (p == (Header *)0)
				return (void *)0;
		}
	}
}

/* Re-inserts the block 'ap' into the free list at the right place (sorted by
 * address) and merges it with either of its now-adjacent free neighbours. */
void free(void *ap)
{
	if (!ap)
		return;

	Header *bp = (Header *)ap - 1;
	Header *p  = freep;

	for (; !(bp > p && bp < p->s.next); p = p->s.next) {
		if (p >= p->s.next && (bp > p || bp < p->s.next))
			break;                                    /* wrap of the ring */
	}

	if (bp + bp->s.size == p->s.next) {                       /* merge above */
		bp->s.size  += p->s.next->s.size;
		bp->s.next   = p->s.next->s.next;
	} else {
		bp->s.next   = p->s.next;
	}
	if (p + p->s.size == bp) {                                /* merge below */
		p->s.size += bp->s.size;
		p->s.next  = bp->s.next;
	} else {
		p->s.next  = bp;
	}
	freep = p;
}

void *calloc(unsigned int n, unsigned int size)
{
	unsigned int bytes = n * size;
	if (size != 0 && bytes / size != n)        /* multiplication overflow */
		return (void *)0;

	void *p = malloc(bytes);
	if (p)
		memset(p, 0, bytes);
	return p;
}

void *realloc(void *old, unsigned int new_size)
{
	if (!old)
		return malloc(new_size);
	if (new_size == 0) {
		free(old);
		return (void *)0;
	}

	Header *bp = (Header *)old - 1;
	unsigned int old_payload = (bp->s.size - 1) * sizeof(Header);
	if (new_size <= old_payload)
		return old;                        /* shrinks (or fits): keep block */

	void *new_p = malloc(new_size);
	if (!new_p)
		return (void *)0;
	memcpy(new_p, old, old_payload);
	free(old);
	return new_p;
}
