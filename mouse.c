/* PS/2 mouse driver: talks to the auxiliary port of the 8042 keyboard
 * controller and decodes the standard 3-byte relative-motion packet. */
#include "mouse.h"
#include "io.h"
#include "fb.h"

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

#define PS2_CMD_ENABLE_AUX   0xA8
#define PS2_CMD_READ_CONFIG  0x20
#define PS2_CMD_WRITE_CONFIG 0x60
#define PS2_CMD_WRITE_AUX    0xD4

#define MOUSE_SET_DEFAULTS 0xF6
#define MOUSE_ENABLE_DATA  0xF4

static unsigned char packet[3];
static int packet_cycle;

static int cur_x, cur_y;
static unsigned char cur_buttons;
static unsigned int max_x, max_y;

/* The cursor is a 7-pixel cross: a vertical arm (7 px) plus a horizontal arm
 * (7 px) sharing the centre pixel, 13 pixels total. Saving exactly what was
 * under those pixels lets us restore the background without a full repaint. */
#define CURSOR_PIXELS 13
static unsigned int cursor_save[CURSOR_PIXELS];
static int cursor_saved_x, cursor_saved_y;
static int cursor_is_drawn;

static void ps2_wait_write(void)
{
	for (unsigned int spins = 0; spins < 100000; spins++)
		if (!(inb(PS2_STATUS) & 0x02))
			return;
}

static void ps2_wait_read(void)
{
	for (unsigned int spins = 0; spins < 100000; spins++)
		if (inb(PS2_STATUS) & 0x01)
			return;
}

static void aux_send(unsigned char value)
{
	ps2_wait_write();
	outb(PS2_CMD, PS2_CMD_WRITE_AUX);
	ps2_wait_write();
	outb(PS2_DATA, value);
	ps2_wait_read();
	inb(PS2_DATA);          /* discard the ACK/resend byte */
}

void mouse_init(unsigned int screen_w, unsigned int screen_h)
{
	max_x = screen_w - 1;
	max_y = screen_h - 1;
	cur_x = (int)(screen_w / 2);
	cur_y = (int)(screen_h / 2);
	cur_buttons = 0;
	packet_cycle = 0;

	ps2_wait_write();
	outb(PS2_CMD, PS2_CMD_ENABLE_AUX);

	ps2_wait_write();
	outb(PS2_CMD, PS2_CMD_READ_CONFIG);
	ps2_wait_read();
	unsigned char config = inb(PS2_DATA);
	config |= 0x02;          /* unmask the second port's IRQ line */
	config &= ~0x20U;        /* make sure the aux port clock isn't disabled */

	ps2_wait_write();
	outb(PS2_CMD, PS2_CMD_WRITE_CONFIG);
	ps2_wait_write();
	outb(PS2_DATA, config);

	aux_send(MOUSE_SET_DEFAULTS);
	aux_send(MOUSE_ENABLE_DATA);
}

void mouse_handle(unsigned char data)
{
	packet[packet_cycle] = data;

	if (packet_cycle == 0 && !(data & 0x08)) {
		/* The always-1 bit (bit 3) is missing: not the start of a real
		 * packet, most likely the stream is out of sync. Drop the byte. */
		return;
	}

	if (packet_cycle < 2) {
		packet_cycle++;
		return;
	}
	packet_cycle = 0;

	if (packet[0] & 0xC0) {
		/* X or Y overflow: the motion in this packet is meaningless. */
		cur_buttons = packet[0] & 0x07;
		return;
	}

	int dx = packet[1];
	int dy = packet[2];
	if (packet[0] & 0x10)
		dx -= 0x100;
	if (packet[0] & 0x20)
		dy -= 0x100;

	cur_buttons = packet[0] & 0x07;
	cur_x += dx;
	cur_y -= dy;             /* PS/2 reports +Y as "up"; screen Y grows down */

	if (cur_x < 0)
		cur_x = 0;
	if (cur_y < 0)
		cur_y = 0;
	if (cur_x > (int)max_x)
		cur_x = (int)max_x;
	if (cur_y > (int)max_y)
		cur_y = (int)max_y;
}

void mouse_get_state(int *x, int *y, unsigned char *buttons)
{
	*x = cur_x;
	*y = cur_y;
	*buttons = cur_buttons;
}

void mouse_draw_cursor(unsigned int rgb)
{
	int idx = 0;

	for (int i = -3; i <= 3; i++)
		cursor_save[idx++] = fb_getpixel((unsigned int)cur_x, (unsigned int)(cur_y + i));
	for (int i = -3; i <= 3; i++) {
		if (i == 0)
			continue;       /* centre pixel already saved by the vertical pass */
		cursor_save[idx++] = fb_getpixel((unsigned int)(cur_x + i), (unsigned int)cur_y);
	}
	cursor_saved_x = cur_x;
	cursor_saved_y = cur_y;
	cursor_is_drawn = 1;

	for (int i = -3; i <= 3; i++) {
		fb_putpixel((unsigned int)cur_x, (unsigned int)(cur_y + i), rgb);
		fb_putpixel((unsigned int)(cur_x + i), (unsigned int)cur_y, rgb);
	}
}

void mouse_undraw_cursor(void)
{
	if (!cursor_is_drawn)
		return;

	int idx = 0;
	for (int i = -3; i <= 3; i++)
		fb_putpixel((unsigned int)cursor_saved_x, (unsigned int)(cursor_saved_y + i), cursor_save[idx++]);
	for (int i = -3; i <= 3; i++) {
		if (i == 0)
			continue;
		fb_putpixel((unsigned int)(cursor_saved_x + i), (unsigned int)cursor_saved_y, cursor_save[idx++]);
	}
	cursor_is_drawn = 0;
}
