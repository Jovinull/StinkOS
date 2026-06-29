/* VFS multi-mount end-to-end validator. Registers a second StinkFS
 * region at slot 1 (B:) on drive 0, in the unused LBA range past the
 * primary StinkFS. Writes a marker file at B:hello, reads it back via
 * the prefix-routed VFS API, asserts byte-equal. Then deletes it and
 * verifies the read fails.
 *
 * Output keys for tools/smoke-vfs-mounts.py:
 *   "mount: sys_mount(1) ok"
 *   "mount: wrote B:hello bytes=N"
 *   "mount: read B:hello bytes=N marker=DEADBEEF"
 *   "mount: PASS roundtrip"
 *
 * The carved region (LBA 250000..250100) sits well past FS_DATA_END
 * (200514) and below the disk size used by the Makefile image. */
#include "libstink.h"

/* The Makefile pads os.bin out to DISK_END = FS_DATA_END * 512
 * (= LBA 200514). The primary StinkFS uses LBAs [128, 158553) today,
 * so [199000, 200000) is well past every committed file but still
 * inside the disk image. */
#define B_DIR_LBA     199000u
#define B_DATA_LBA    199002u
#define B_DATA_END    200000u
#define MARKER        0xDEADBEEFu

void main(void)
{
	int rc = sys_mount(1, 0, B_DIR_LBA, B_DATA_LBA, B_DATA_END);
	if (rc != 0) {
		sys_printf("mount: FAIL sys_mount rc=%d\n", rc);
		return;
	}
	sys_log("mount: sys_mount(1) ok");

	unsigned int marker = MARKER;
	int wrote = sys_fwrite("B:hello", &marker, sizeof(marker));
	if (wrote != 0) {
		sys_printf("mount: FAIL write rc=%d\n", wrote);
		return;
	}
	sys_printf("mount: wrote B:hello bytes=%u\n", (unsigned)sizeof(marker));

	unsigned int got = 0;
	int n = sys_fread("B:hello", &got, sizeof(got));
	if (n != (int)sizeof(got)) {
		sys_printf("mount: FAIL read n=%d\n", n);
		return;
	}
	sys_printf("mount: read B:hello bytes=%d marker=%X\n", n, got);

	if (got != MARKER) {
		sys_log("mount: FAIL marker mismatch");
		return;
	}

	/* Cleanup: delete the test file so re-runs don't accumulate. */
	int del = sys_fdelete("B:hello");
	if (del != 0) {
		sys_printf("mount: FAIL delete rc=%d\n", del);
		return;
	}

	/* Confirm delete worked: read should now fail. */
	int n2 = sys_fread("B:hello", &got, sizeof(got));
	if (n2 >= 0) {
		sys_log("mount: FAIL read-after-delete returned data");
		return;
	}

	sys_log("mount: PASS roundtrip");
}
