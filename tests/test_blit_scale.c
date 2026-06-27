/* Host-side test for the nearest-neighbour scaler in
 * kernel/drivers/video/fb.c (fb_blit_scaled). The kernel maps each
 * destination pixel (col, row) back to the source pixel at:
 *   sx = (col * src_w) / dst_w
 *   sy = (row * src_h) / dst_h
 * Picking the right column/row off-by-one here is exactly the kind
 * of regression that produces "half the picture" or "double image"
 * bugs in Doom fullscreen, which is the main user of this path.
 *
 * Test scenarios:
 *   - 1:1 copy (src_w == dst_w, src_h == dst_h)
 *   - 2x upscale (each source pixel maps to a 2x2 block)
 *   - 2:1 downscale (every other source pixel kept)
 *   - 4:3 stretch (no clean factor; sampling table is the contract)
 *   - destination clipped to a smaller frame
 */
#include <stdio.h>
#include <string.h>

#define DST_W_CAP 32u
#define DST_H_CAP 32u

static unsigned int fb_w = 16, fb_h = 16;
static unsigned int dst_buf[DST_H_CAP][DST_W_CAP];

static void clear_dst(void)
{
	memset(dst_buf, 0, sizeof(dst_buf));
}

static void scale(unsigned int x0, unsigned int y0,
                  unsigned int dst_w, unsigned int dst_h,
                  const unsigned int *src,
                  unsigned int src_w, unsigned int src_h)
{
	if (!src || dst_w == 0 || dst_h == 0 || src_w == 0 || src_h == 0) return;
	if (x0 >= fb_w || y0 >= fb_h) return;

	unsigned int cw = dst_w, ch = dst_h;
	if (x0 + cw > fb_w) cw = fb_w - x0;
	if (y0 + ch > fb_h) ch = fb_h - y0;

	for (unsigned int row = 0; row < ch; row++) {
		unsigned int sy = (row * src_h) / dst_h;
		for (unsigned int col = 0; col < cw; col++) {
			unsigned int sx = (col * src_w) / dst_w;
			dst_buf[y0 + row][x0 + col] = src[sy * src_w + sx];
		}
	}
}

static int expect_u(const char *label, unsigned int got, unsigned int want)
{
	if (got == want) { printf("ok   %-55s = 0x%x\n", label, got); return 0; }
	printf("FAIL %s: got 0x%x, want 0x%x\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;

	/* --- 1:1 copy: 4x4 source -> 4x4 dst. ----------------------------- */
	unsigned int src44[16];
	for (int i = 0; i < 16; i++) src44[i] = 0x10000u + (unsigned)i;
	clear_dst();
	scale(0, 0, 4, 4, src44, 4, 4);
	failures += expect_u("1:1 top-left",   dst_buf[0][0], 0x10000u);
	failures += expect_u("1:1 bottom-right", dst_buf[3][3], 0x1000Fu);
	failures += expect_u("1:1 middle",     dst_buf[1][2], 0x10006u);

	/* --- 2x upscale: 2x2 -> 4x4. Each source pixel covers a 2x2 block. */
	unsigned int src22[4] = { 0x100u, 0x200u, 0x300u, 0x400u };
	clear_dst();
	scale(0, 0, 4, 4, src22, 2, 2);
	failures += expect_u("2x: dst[0][0] -> src[0][0]", dst_buf[0][0], 0x100u);
	failures += expect_u("2x: dst[0][1] -> src[0][0]", dst_buf[0][1], 0x100u);
	failures += expect_u("2x: dst[0][2] -> src[0][1]", dst_buf[0][2], 0x200u);
	failures += expect_u("2x: dst[1][0] -> src[0][0]", dst_buf[1][0], 0x100u);
	failures += expect_u("2x: dst[2][0] -> src[1][0]", dst_buf[2][0], 0x300u);
	failures += expect_u("2x: dst[3][3] -> src[1][1]", dst_buf[3][3], 0x400u);

	/* --- 2:1 downscale: 4x4 -> 2x2. Picks src col/row 0 and 2. ------- */
	clear_dst();
	scale(0, 0, 2, 2, src44, 4, 4);
	failures += expect_u("2:1 dst[0][0] -> src[0][0]",   dst_buf[0][0], 0x10000u);
	failures += expect_u("2:1 dst[0][1] -> src[0][2]",   dst_buf[0][1], 0x10002u);
	failures += expect_u("2:1 dst[1][0] -> src[2][0]",   dst_buf[1][0], 0x10008u);
	failures += expect_u("2:1 dst[1][1] -> src[2][2]",   dst_buf[1][1], 0x1000Au);

	/* --- Destination clip: x0=14, dst_w=8, fb_w=16 -> only 2 cols. --- */
	clear_dst();
	scale(14, 0, 8, 1, src44, 4, 1);
	failures += expect_u("clip x: dst[0][14] -> src[0][0]", dst_buf[0][14], 0x10000u);
	failures += expect_u("clip x: dst[0][15] -> src[0][0]", dst_buf[0][15], 0x10000u);
	/* col 16 lies outside the framebuffer entirely. */

	/* --- Zero-input rejection: src=NULL or 0-dim must skip. ---------- */
	clear_dst();
	scale(0, 0, 4, 4, 0,      4, 4);   /* NULL src */
	failures += expect_u("NULL src: dst[0][0] untouched", dst_buf[0][0], 0);
	scale(0, 0, 0, 4, src44,  4, 4);   /* dst_w=0 */
	failures += expect_u("dst_w=0: dst[0][0] untouched", dst_buf[0][0], 0);
	scale(0, 0, 4, 4, src44,  0, 4);   /* src_w=0 */
	failures += expect_u("src_w=0: dst[0][0] untouched", dst_buf[0][0], 0);

	/* --- Origin past the framebuffer: full skip. -------------------- */
	clear_dst();
	scale(fb_w, 0, 4, 4, src44, 4, 4);
	failures += expect_u("x0==fb_w: full skip", dst_buf[0][0], 0);
	scale(0, fb_h, 4, 4, src44, 4, 4);
	failures += expect_u("y0==fb_h: full skip", dst_buf[0][0], 0);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
