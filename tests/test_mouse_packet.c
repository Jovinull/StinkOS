/* Host-side test for the PS/2 mouse 3-byte packet decoder in
 * kernel/drivers/input/mouse.c. The protocol is finicky:
 *
 *   byte 0: [YO XO YS XS 1 MB RB LB]
 *     bit 3 is "always 1"; if it's 0 on cycle 0 the stream is out of
 *     sync, so the byte is dropped.
 *     bits 4-5 (XS,YS) extend bytes 1-2 to signed 9-bit deltas
 *     (subtract 0x100 when set).
 *     bits 6-7 (XO,YO) flag delta overflow -- the motion in this
 *     packet is meaningless and must be zeroed.
 *   byte 1: dx (signed via byte0 bit 4)
 *   byte 2: dy (signed via byte0 bit 5; PS/2 +Y = up, screen +Y = down,
 *     so the kernel negates it for the cursor / accumulator).
 *
 * If any of those sign / overflow / sync gates regress, the cursor
 * drifts diagonally on real hardware (silent in QEMU because QEMU
 * never sets the overflow or sync-loss bits).
 */
#include <stdio.h>

struct decoded {
	int           accepted;          /* 1 if packet completed, 0 if dropped/in-progress */
	int           dx;                /* screen-space dx applied */
	int           dy;                /* screen-space dy applied (PS/2 negated) */
	unsigned char buttons;
	int           overflow;          /* 1 if overflow bit forced zero-motion */
};

/* Mirror of mouse_handle's accumulation. We drive it byte-by-byte and
 * report what the third-byte completion would have produced. */
static unsigned char packet[3];
static int packet_cycle;

static int feed_byte(unsigned char data, struct decoded *out)
{
	packet[packet_cycle] = data;

	if (packet_cycle == 0 && !(data & 0x08)) {
		/* sync-loss: drop the byte, don't advance */
		return 0;
	}
	if (packet_cycle < 2) {
		packet_cycle++;
		return 0;
	}
	packet_cycle = 0;

	out->accepted = 1;
	out->buttons  = packet[0] & 0x07;
	if (packet[0] & 0xC0) {
		out->overflow = 1;
		out->dx = 0;
		out->dy = 0;
		return 1;
	}
	out->overflow = 0;
	int dx = packet[1];
	int dy = packet[2];
	if (packet[0] & 0x10) dx -= 0x100;
	if (packet[0] & 0x20) dy -= 0x100;
	out->dx =  dx;
	out->dy = -dy;       /* screen Y grows down; PS/2 +Y means up */
	return 1;
}

static void reset_packet(void)
{
	packet_cycle = 0;
	packet[0] = packet[1] = packet[2] = 0;
}

static int expect_int(const char *label, int got, int want)
{
	if (got == want) { printf("ok   %-55s = %d\n", label, got); return 0; }
	printf("FAIL %s: got %d, want %d\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;
	struct decoded d;

	/* --- well-formed packet: small positive motion ----------- */
	reset_packet();
	/* byte0 = 0b0010_1000: always-1 bit set, YS=1 to make dy a 9-bit
	 * negative (so it represents PS/2 +5 = "up", which the kernel
	 * negates to screen +5 = "down"). */
	feed_byte(0x28, &d);
	feed_byte(0x05, &d);                 /* dx = +5 */
	int got = feed_byte(0xFB, &d);       /* dy raw = 0xFB - 0x100 = -5 -> screen +5 */
	failures += expect_int("3rd byte completes packet", got, 1);
	failures += expect_int("dx == +5",                 d.dx, 5);
	failures += expect_int("dy == +5 (PS/2 -5 negated)", d.dy, 5);
	failures += expect_int("no overflow flag",         d.overflow, 0);
	failures += expect_int("no buttons",               d.buttons, 0);

	/* --- negative X delta via XS bit ---------------------- */
	reset_packet();
	feed_byte(0x18, &d);                 /* always-1=1, XS=1 */
	feed_byte(0xF0, &d);                 /* dx = 0xF0 - 0x100 = -16 */
	feed_byte(0x00, &d);                 /* dy = 0 */
	failures += expect_int("dx with XS set -> -16",    d.dx, -16);
	failures += expect_int("dy == 0",                  d.dy, 0);

	/* --- negative Y delta via YS bit ---------------------- */
	reset_packet();
	feed_byte(0x28, &d);                 /* always-1=1, YS=1 */
	feed_byte(0x00, &d);
	feed_byte(0xF0, &d);                 /* dy raw = 0xF0 - 0x100 = -16, screen = +16 */
	failures += expect_int("dy with YS set (raw -16) -> screen +16",
	                       d.dy, 16);

	/* --- both signs at once + buttons ------------------- */
	reset_packet();
	feed_byte(0x39, &d);                 /* always-1, XS, YS, LB */
	feed_byte(0xFF, &d);                 /* dx raw = -1 */
	feed_byte(0xFE, &d);                 /* dy raw = -2 -> screen +2 */
	failures += expect_int("dx -1",                    d.dx, -1);
	failures += expect_int("dy +2",                    d.dy, 2);
	failures += expect_int("LB pressed (buttons=1)",   d.buttons, 1);

	/* --- overflow: bits 6/7 zero the motion ----------- */
	reset_packet();
	feed_byte(0x48, &d);                 /* always-1, XO */
	feed_byte(0x55, &d);
	feed_byte(0x55, &d);
	failures += expect_int("XO -> dx zeroed",          d.dx, 0);
	failures += expect_int("XO -> dy zeroed",          d.dy, 0);
	failures += expect_int("XO -> overflow flag set",  d.overflow, 1);

	reset_packet();
	feed_byte(0x88, &d);                 /* always-1, YO */
	feed_byte(0x7F, &d);
	feed_byte(0x7F, &d);
	failures += expect_int("YO -> dx zeroed",          d.dx, 0);
	failures += expect_int("YO -> overflow flag set",  d.overflow, 1);

	/* --- sync loss: byte0 missing the always-1 bit dropped */
	reset_packet();
	int r = feed_byte(0x00, &d);         /* bit 3 == 0 -> drop */
	failures += expect_int("sync-loss byte dropped",   r, 0);
	failures += expect_int("packet_cycle stays 0 after drop",
	                       packet_cycle, 0);
	/* Now feed a real packet -- should still complete clean. */
	feed_byte(0x08, &d);
	feed_byte(0x03, &d);
	r = feed_byte(0x02, &d);
	failures += expect_int("after sync recovery: packet completes", r, 1);
	failures += expect_int("dx +3", d.dx, 3);
	failures += expect_int("dy -2", d.dy, -2);

	/* --- only middle/right buttons ------------------------- */
	reset_packet();
	feed_byte(0x0E, &d);                 /* always-1, MB, RB */
	feed_byte(0x00, &d);
	feed_byte(0x00, &d);
	failures += expect_int("MB+RB buttons mask = 0x06",
	                       d.buttons, 0x06);

	/* --- max-positive delta (0x7F, no XS) ---------------- */
	reset_packet();
	feed_byte(0x08, &d);
	feed_byte(0x7F, &d);                 /* +127 */
	feed_byte(0x01, &d);
	failures += expect_int("dx max positive = +127", d.dx, 127);

	/* --- max-negative delta (0x80 with XS) -------------- */
	reset_packet();
	feed_byte(0x18, &d);                 /* XS=1 */
	feed_byte(0x80, &d);                 /* 0x80 - 0x100 = -128 */
	feed_byte(0x00, &d);
	failures += expect_int("dx max negative = -128", d.dx, -128);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
