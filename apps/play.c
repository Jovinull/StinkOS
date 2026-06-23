/* StinkOS userland C app: a small grid collector game that ties the whole
 * platform together -- graphics (SYS_DRAW), keyboard (SYS_GETKEY), sound
 * (SYS_SOUND), the timer (SYS_TICKS) and the filesystem (SYS_FREAD/FWRITE).
 * Move the green block with w/a/s/d onto the yellow food to score; 'q' ends the
 * game, which then keeps the best score in the StinkFS file "hiscore". */
#include "libstink.h"

#define CELL 20
#define SZ   16
#define BG   0x001022

static void fill_cell(int col, int row, unsigned int color)
{
	int x0 = col * CELL + 2;
	int y0 = row * CELL + 2;
	for (int dy = 0; dy < SZ; dy++)
		for (int dx = 0; dx < SZ; dx++)
			sys_draw(x0 + dx, y0 + dy, color);
}

static void blip(void)
{
	sys_sound(880);
	unsigned int t = sys_ticks();
	while (sys_ticks() - t < 3)
		;                          /* short tone, paced by the timer */
	sys_sound(0);
}

void main(void)
{
	static const int food_col[3] = { 11, 11, 10 };
	static const int food_row[3] = { 10, 11, 11 };

	int pcol = 10, prow = 10;
	int fi = 0;
	int score = 0;

	fill_cell(food_col[fi], food_row[fi], 0xFFFF00);   /* food   */
	fill_cell(pcol, prow, 0x00FF00);                   /* player */

	for (;;) {
		int c = sys_getkey();
		if (c == 0)
			continue;
		if (c == 'q')
			break;

		int ncol = pcol, nrow = prow;
		if (c == 'w')      nrow--;
		else if (c == 's') nrow++;
		else if (c == 'a') ncol--;
		else if (c == 'd') ncol++;
		else continue;

		if (ncol < 0 || nrow < 0 || ncol > 50 || nrow > 36)
			continue;                          /* off the field */

		fill_cell(pcol, prow, BG);                 /* erase old position */
		pcol = ncol;
		prow = nrow;
		fill_cell(pcol, prow, 0x00FF00);

		if (fi < 3 && pcol == food_col[fi] && prow == food_row[fi]) {
			score++;
			blip();
			fi++;
			if (fi < 3)
				fill_cell(food_col[fi], food_row[fi], 0xFFFF00);
		}
	}

	sys_log("game over");

	unsigned int high = 0;
	char buf[4];
	if (sys_fread("hiscore", buf, 4) == 4)
		high = *(unsigned int *)buf;

	if ((unsigned int)score > high) {
		*(unsigned int *)buf = (unsigned int)score;
		sys_fwrite("hiscore", buf, 4);
		sys_log("game: new high");
	} else {
		sys_log("game: high kept");
	}

	while (sys_getkey() == 0)
		;
}
