/* StinkOS userland C app: moves a block around the screen with the arrow
 * keys (KEY_UP/DOWN/LEFT/RIGHT from libstink.h), exercising the keyboard
 * driver's new extended-scancode decoding instead of the WASD convention the
 * other demos use. Press 'q' to return to the menu.
 *
 * NOT YET ON THE BOOT MENU: needs a disk slot + TOC entry in the Makefile,
 * which is Core's (Equipe 1's) jurisdiction. See the pending request in
 * TASKS.md. */
#include "libstink.h"

#define BLOCK 16
#define STEP 8
#define SCREEN_W 1024
#define SCREEN_H 768
#define BG 0x001022

static void draw_block(int x, int y, unsigned int rgb)
{
	for (int dy = 0; dy < BLOCK; dy++)
		for (int dx = 0; dx < BLOCK; dx++)
			sys_draw(x + dx, y + dy, rgb);
}

void main(void)
{
	int x = (SCREEN_W - BLOCK) / 2;
	int y = (SCREEN_H - BLOCK) / 2;

	sys_log("arrows app running");
	draw_block(x, y, 0x00FFAA);

	for (;;) {
		int c = sys_getkey();
		if (c == 0)
			continue;
		if (c == 'q') {
			sys_log("arrows: quit");
			return;
		}

		int nx = x, ny = y;
		if (c == KEY_UP && y > 0)
			ny -= STEP;
		else if (c == KEY_DOWN && y < SCREEN_H - BLOCK)
			ny += STEP;
		else if (c == KEY_LEFT && x > 0)
			nx -= STEP;
		else if (c == KEY_RIGHT && x < SCREEN_W - BLOCK)
			nx += STEP;
		else
			continue;

		draw_block(x, y, BG);
		x = nx;
		y = ny;
		draw_block(x, y, 0x00FFAA);
	}
}
