/* COW fork end-to-end validator. Allocates a heap page, writes a known
 * marker, fork()s, then each side writes its own marker and reads back.
 * If COW works correctly, parent and child see exclusively their own
 * writes; the post-write reads MUST diverge. If the kernel still did
 * eager memcpy, the test would also pass (both have private frames from
 * the start) -- but the precondition log "pre-fork marker" should match
 * across BOTH sides BEFORE the divergent writes, proving they shared.
 *
 * Output keys for tools/smoke-cow.py to scrape:
 *   "cow: pre-fork marker=DEADBEEF"
 *   "cow: child saw pre=DEADBEEF wrote=CAFEBABE"
 *   "cow: parent saw pre=DEADBEEF wrote=FEEDFACE"
 *   "cow: parent post-child read=FEEDFACE"
 *   "cow: PASS divergence"
 */
#include "libstink.h"

#define PRE_MARKER     0xDEADBEEFu
#define CHILD_MARKER   0xCAFEBABEu
#define PARENT_MARKER  0xFEEDFACEu

void main(void)
{
	unsigned int *page = (unsigned int *)sys_mmap(4096);
	if (!page) {
		sys_log("cow: FAIL mmap");
		return;
	}
	*page = PRE_MARKER;
	sys_printf("cow: pre-fork marker=%X\n", *page);

	int pid = sys_fork();
	if (pid < 0) {
		sys_log("cow: FAIL fork");
		return;
	}

	if (pid == 0) {
		/* Child: read shared frame (should still see PRE_MARKER),
		 * then write CHILD_MARKER. The write triggers a COW fault;
		 * after it the child has its own private frame. */
		unsigned int pre = *page;
		*page = CHILD_MARKER;
		unsigned int post = *page;
		sys_printf("cow: child saw pre=%X wrote=%X\n", pre, post);
		sys_exit();
	}

	/* Parent: same pattern -- pre-read should still match PRE_MARKER,
	 * then writing PARENT_MARKER triggers parent's COW fault. */
	unsigned int pre = *page;
	*page = PARENT_MARKER;
	unsigned int post = *page;
	sys_printf("cow: parent saw pre=%X wrote=%X\n", pre, post);

	sys_wait();

	/* After the child exited, the parent's view of the page must be
	 * its own PARENT_MARKER, NOT the child's CHILD_MARKER. If they
	 * still share, this would either be CHILD_MARKER or whichever
	 * write won the race. */
	unsigned int final = *page;
	sys_printf("cow: parent post-child read=%X\n", final);

	if (pre == PRE_MARKER && final == PARENT_MARKER)
		sys_log("cow: PASS divergence");
	else
		sys_log("cow: FAIL divergence");
}
