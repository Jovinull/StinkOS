/* Host-side test for the SYS_BLIT / SYS_BLIT_SCALED integer-overflow
 * guard in kernel/sys/syscall.c. Userland passes 16-bit width / height
 * fields, so the product `w * h * 4` can wrap 32-bit unsigned for any
 * pair whose area exceeds 2^30 pixels. Without the round-trip check a
 * wrapped (small) byte count would let paging_user_range_ok approve a
 * range that the framebuffer then walks over for gigabytes of memory.
 *
 * Guard formula (mirrored below):
 *   bytes = w * h * 4
 *   reject if   w == 0 || h == 0 || (bytes / 4 / w) != h
 *
 * The round-trip check is what catches the wraparound: in unsigned
 * arithmetic, `(w*h*4) / 4 / w` reproduces `h` only when nothing
 * overflowed; any wrap turns the result into a different value.
 */
#include <stdio.h>

static int blit_accepts(unsigned int w, unsigned int h)
{
	if (w == 0 || h == 0) return 0;
	unsigned int bytes = w * h * 4u;
	if (bytes / 4u / w != h) return 0;
	return 1;
}

static int expect_int(const char *label, int got, int want)
{
	if (got == want) { printf("ok   %-50s = %d\n", label, got); return 0; }
	printf("FAIL %s: got %d, want %d\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;

	/* Empty geometry rejected. */
	failures += expect_int("0x0 rejected",                blit_accepts(0, 0),         0);
	failures += expect_int("100x0 rejected",              blit_accepts(100, 0),       0);
	failures += expect_int("0x100 rejected",              blit_accepts(0, 100),       0);

	/* Typical sizes accepted. */
	failures += expect_int("1x1 accepted",                blit_accepts(1, 1),         1);
	failures += expect_int("64x64 accepted",              blit_accepts(64, 64),       1);
	failures += expect_int("640x480 accepted",            blit_accepts(640, 480),     1);
	failures += expect_int("1024x768 accepted",           blit_accepts(1024, 768),    1);
	failures += expect_int("1920x1080 accepted",          blit_accepts(1920, 1080),   1);

	/* Boundary: exactly 2^30 pixels (4 GB / 4 B per pixel - 1 pixel).
	 * 16384 * 16384 = 2^28 pixels = 1 GB -- well under overflow. */
	failures += expect_int("16384x16384 accepted",        blit_accepts(16384, 16384), 1);

	/* Boundary: 32768 * 32768 = 2^30 pixels * 4 B = 4 GB. The
	 * multiplication overflows 32-bit to 0; the guard must reject. */
	failures += expect_int("32768x32768 (4 GB) rejected", blit_accepts(32768, 32768), 0);

	/* Worst case the wire allows: 65535 * 65535 * 4 = 17.18 GB. */
	failures += expect_int("65535x65535 rejected",        blit_accepts(65535, 65535), 0);

	/* Lopsided overflow: tiny height, huge width. */
	failures += expect_int("65535x2 accepted (<4 GB)",    blit_accepts(65535, 2),     1);
	failures += expect_int("65535x65000 rejected",        blit_accepts(65535, 65000), 0);

	/* Subtle case: 65536 * 16384 * 4 = exactly 4 GB -> wraps to 0 ->
	 * 0 / 4 / 65536 = 0 != 16384 -> rejected. */
	failures += expect_int("65536x16384 (=4 GB) rejected", blit_accepts(65536, 16384), 0);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
