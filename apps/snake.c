/* StinkOS userland C app: classic Snake. Move with the arrow keys, eat the
 * food to grow and score, avoid the walls and your own tail. 'q' quits back
 * to the menu. Exercises the keyboard's extended-scancode arrows, sys_draw,
 * sys_sound and sys_ticks together in one app.
 *
 * NOT YET ON THE BOOT MENU: needs a disk slot + TOC entry in the Makefile,
 * which is Core's (Equipe 1's) jurisdiction. See the pending request in
 * TASKS.md. */
#include "libstink.h"

#define CELL       16
#define CELLS_W    64           /* 1024 / CELL */
#define CELLS_H    48           /*  768 / CELL */
#define MAX_LEN    200
#define MOVE_TICKS 8             /* board steps once per this many sys_ticks */
#define BG         0x001022
#define SNAKE_RGB  0x00FF66
#define FOOD_RGB   0xFF4040

enum { DIR_UP = 0, DIR_DOWN, DIR_LEFT, DIR_RIGHT };
static const int dx[4] = { 0,  0, -1, 1 };
static const int dy[4] = { -1, 1,  0, 0 };

struct cell { unsigned char x, y; };
static struct cell body[MAX_LEN];
static int len;

static void draw_cell(int cx, int cy, unsigned int rgb)
{
	int px = cx * CELL;
	int py = cy * CELL;
	for (int y = 0; y < CELL; y++)
		for (int x = 0; x < CELL; x++)
			sys_draw(px + x, py + y, rgb);
}

static int occupied(int x, int y)
{
	for (int i = 0; i < len; i++)
		if (body[i].x == x && body[i].y == y)
			return 1;
	return 0;
}

static int food_x, food_y;

static void place_food(void)
{
	do {
		food_x = rand() % CELLS_W;
		food_y = rand() % CELLS_H;
	} while (occupied(food_x, food_y));
	draw_cell(food_x, food_y, FOOD_RGB);
}

void main(void)
{
	srand(sys_ticks());

	len = 3;
	body[0].x = CELLS_W / 2;
	body[0].y = CELLS_H / 2;
	body[1].x = body[0].x - 1;
	body[1].y = body[0].y;
	body[2].x = body[0].x - 2;
	body[2].y = body[0].y;

	int dir = DIR_RIGHT;
	int pending = DIR_RIGHT;
	int score = 0;

	for (int i = 0; i < len; i++)
		draw_cell(body[i].x, body[i].y, SNAKE_RGB);
	place_food();

	sys_log("snake: running");
	unsigned int last_move = sys_ticks();

	for (;;) {
		int c = sys_getkey();
		if (c == 'q') {
			sys_log("snake: quit");
			return;
		}
		if (c == KEY_UP && dir != DIR_DOWN)
			pending = DIR_UP;
		else if (c == KEY_DOWN && dir != DIR_UP)
			pending = DIR_DOWN;
		else if (c == KEY_LEFT && dir != DIR_RIGHT)
			pending = DIR_LEFT;
		else if (c == KEY_RIGHT && dir != DIR_LEFT)
			pending = DIR_RIGHT;

		unsigned int now = sys_ticks();
		if (now - last_move < MOVE_TICKS)
			continue;
		last_move = now;
		dir = pending;

		int nx = body[0].x + dx[dir];
		int ny = body[0].y + dy[dir];

		if (nx < 0 || nx >= CELLS_W || ny < 0 || ny >= CELLS_H) {
			sys_printf("snake: game over, score %d (hit wall)", score);
			return;
		}
		/* The tail cell is about to move away, so colliding with it is
		 * fine -- only check the body excluding the current tail. */
		int hit_self = 0;
		for (int i = 0; i < len - 1; i++)
			if (body[i].x == nx && body[i].y == ny)
				hit_self = 1;
		if (hit_self) {
			sys_printf("snake: game over, score %d (hit self)", score);
			return;
		}

		int ate = (nx == food_x && ny == food_y);
		struct cell old_tail = body[len - 1];

		if (ate && len < MAX_LEN)
			len++;
		for (int i = len - 1; i > 0; i--)
			body[i] = body[i - 1];
		body[0].x = (unsigned char)nx;
		body[0].y = (unsigned char)ny;

		if (!ate)
			draw_cell(old_tail.x, old_tail.y, BG);
		draw_cell(body[0].x, body[0].y, SNAKE_RGB);

		if (ate) {
			score++;
			sys_sound(1200);
			unsigned int t0 = sys_ticks();
			while (sys_ticks() - t0 < 4)
				;
			sys_sound(0);
			place_food();
		}
	}
}
