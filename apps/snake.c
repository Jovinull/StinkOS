/* StinkOS userland app: classic Snake. Arrow keys to move, 'p' to pause,
 * 'q' to quit, 'r' to restart after game over. Speed increases every 5 food
 * eaten. Exercises arrows, sys_draw, sys_sound, sys_ticks, and StinkFS (high
 * score persistence). */
#include "libstink.h"

#define CELL        16
#define CELLS_W     64           /* 1024 / CELL */
#define CELLS_H     45           /* (768 - OY) / CELL = 734/16, floored */
#define OY          34           /* titlebar height + separator */
#define MAX_LEN     200
#define MOVE_TICKS  8            /* initial ticks per step (decreases with score) */
#define BG          0x001022
#define SNAKE_RGB   0x00FF66
#define FOOD_RGB    0xFF4040

enum { DIR_UP = 0, DIR_DOWN, DIR_LEFT, DIR_RIGHT };
static const int dx[4] = { 0,  0, -1, 1 };
static const int dy[4] = { -1, 1,  0, 0 };

struct cell { unsigned char x, y; };
static struct cell body[MAX_LEN];
static int len;
static int food_x, food_y;

static void draw_cell(int cx, int cy, unsigned int rgb)
{
	sys_fillrect(cx * CELL, cy * CELL + OY, CELL, CELL, rgb);
}

static int occupied(int x, int y)
{
	for (int i = 0; i < len; i++)
		if (body[i].x == x && body[i].y == y)
			return 1;
	return 0;
}

static void game_over_jingle(void)
{
	sys_tone(880, 6);
	sys_tone(660, 6);
	sys_tone(440, 10);
}

static void report_game_over(int score, const char *reason)
{
	unsigned int high = 0;
	char buf[4];

	if (sys_fread("snakehi", buf, sizeof(buf)) == (int)sizeof(buf))
		high = *(unsigned int *)buf;

	if ((unsigned int)score > high) {
		*(unsigned int *)buf = (unsigned int)score;
		sys_fwrite("snakehi", buf, sizeof(buf));
		sys_printf("snake: NEW HIGH SCORE %d (%s)", score, reason);
	} else {
		sys_printf("snake: game over, score %d, best %u (%s)", score, high, reason);
	}
	game_over_jingle();
}

static void place_food(void)
{
	do {
		food_x = rand() % CELLS_W;
		food_y = rand() % CELLS_H;
	} while (occupied(food_x, food_y));
	draw_cell(food_x, food_y, FOOD_RGB);
}

static void clear_screen(void)
{
	draw_window_frame(0, 0, 1024, 768, "Snake  --  Arrows: move  |  p: pause  q: quit");
	sys_fillrect(0, OY, 1024, 768 - OY, BG);
}

/* Returns the exit reason: 0 = player quit, 1 = hit wall, 2 = hit self.
 * Sets *score_out to the final score. */
static int play_game(int *score_out)
{
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
	int paused = 0;

	clear_screen();
	for (int i = 0; i < len; i++)
		draw_cell(body[i].x, body[i].y, SNAKE_RGB);
	place_food();

	sys_log("snake: running (p=pause q=quit)");
	unsigned int last_move = sys_ticks();

	for (;;) {
		int c = sys_getkey();

		if (c == 'q') {
			*score_out = score;
			return 0;
		}
		if (c == 'p') {
			paused = !paused;
			sys_log(paused ? "snake: paused" : "snake: resumed");
		}
		if (paused)
			continue;

		if (c == KEY_UP    && dir != DIR_DOWN)  pending = DIR_UP;
		else if (c == KEY_DOWN  && dir != DIR_UP)   pending = DIR_DOWN;
		else if (c == KEY_LEFT  && dir != DIR_RIGHT) pending = DIR_LEFT;
		else if (c == KEY_RIGHT && dir != DIR_LEFT)  pending = DIR_RIGHT;

		/* Speed scales with score: faster every 5 food eaten, min 2 ticks. */
		unsigned int move_ticks = (unsigned int)MOVE_TICKS;
		unsigned int boost = (unsigned int)(score / 5);
		if (boost + 2 < move_ticks)
			move_ticks -= boost;
		else
			move_ticks = 2;

		unsigned int now = sys_ticks();
		if (now - last_move < move_ticks)
			continue;
		last_move = now;
		dir = pending;

		int nx = body[0].x + dx[dir];
		int ny = body[0].y + dy[dir];

		if (nx < 0 || nx >= CELLS_W || ny < 0 || ny >= CELLS_H) {
			*score_out = score;
			return 1;
		}
		int hit_self = 0;
		for (int i = 0; i < len - 1; i++)
			if (body[i].x == nx && body[i].y == ny)
				hit_self = 1;
		if (hit_self) {
			*score_out = score;
			return 2;
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
			sys_tone(1200, 4);
			place_food();
		}
	}
}

void main(void)
{
	srand(sys_ticks());

	for (;;) {
		int score = 0;
		int reason = play_game(&score);

		if (reason == 0) {
			sys_log("snake: quit");
			return;
		}

		const char *why = (reason == 1) ? "hit wall" : "hit self";
		report_game_over(score, why);

		sys_log("snake: r=restart  q=quit");
		for (;;) {
			int c = sys_getkey();
			if (c == 'q')
				return;
			if (c == 'r')
				break;
		}
	}
}
