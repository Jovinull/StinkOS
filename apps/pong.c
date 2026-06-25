/* StinkOS userland app: single-player Pong. UP/DOWN arrows (or W/S) control
 * the right paddle; the left paddle is AI. First to 7 points wins. Score
 * logged to serial. 'q' quits to menu at any time. */
#include "libstink.h"

#define W           1024
#define H           768
#define BG          0x001022
#define FG          0xFFFFFF
#define BALL_C      0xFF8800
#define PAD_C       0x00AAFF
#define AI_C        0xFF4444

#define BALL_SZ     14
#define PAD_W       12
#define PAD_H       80
#define MARGIN      24          /* paddle x offset from each edge */
#define SPEED_X     5           /* ball x pixels per step */
#define SPEED_Y     4           /* ball y pixels per step */
#define PAD_SPEED   5           /* paddle pixels per step */
#define STEP_TICKS  2           /* sys_ticks() between game steps */
#define WIN_SCORE   7

static void fill_rect(int x, int y, int w, int h, unsigned int rgb)
{
	for (int row = 0; row < h; row++)
		for (int col = 0; col < w; col++)
			sys_draw(x + col, y + row, rgb);
}

static void clear_rect(int x, int y, int w, int h)
{
	fill_rect(x, y, w, h, BG);
}

static void draw_score(int lscore, int rscore)
{
	sys_printf("pong: %d - %d", lscore, rscore);

	/* Render "L - R" centred on screen using the kernel font (8x8 glyphs). */
	char buf[6];
	buf[0] = '0' + (lscore % 10);
	buf[1] = ' '; buf[2] = '-'; buf[3] = ' ';
	buf[4] = '0' + (rscore % 10);
	buf[5] = '\0';
	/* 5 glyphs × 8px = 40px wide; centre at W/2 = 512 → x = 512 - 20 = 492 */
	sys_drawtext(492, 8, buf, FG);
}

/* Clamps v to [lo, hi]. */
static int clamp(int v, int lo, int hi)
{
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

static int play_round(void)   /* returns 0 = quit, 1 = play again */
{
	int bx = W / 2 - BALL_SZ / 2;
	int by = H / 2 - BALL_SZ / 2;
	int vx = SPEED_X, vy = SPEED_Y;

	int lpad = H / 2 - PAD_H / 2;   /* AI pad y */
	int rpad = H / 2 - PAD_H / 2;   /* player pad y */

	int lscore = 0, rscore = 0;

	/* Clear screen. */
	fill_rect(0, 0, W, H, BG);
	/* Centre dashed line. */
	for (int y = 0; y < H; y += 16)
		fill_rect(W / 2 - 1, y, 2, 8, 0x334466);

	fill_rect(MARGIN, lpad, PAD_W, PAD_H, AI_C);
	fill_rect(W - MARGIN - PAD_W, rpad, PAD_W, PAD_H, PAD_C);
	fill_rect(bx, by, BALL_SZ, BALL_SZ, BALL_C);

	sys_log("pong: running (arrows/WS=paddle, q=quit)");
	unsigned int last = sys_ticks();

	for (;;) {
		int c = sys_getkey();
		if (c == 'q') return 0;

		/* Player paddle. */
		int new_rpad = rpad;
		if ((c == KEY_UP || c == 'w') && rpad > 0)
			new_rpad -= PAD_SPEED;
		if ((c == KEY_DOWN || c == 's') && rpad < H - PAD_H)
			new_rpad += PAD_SPEED;
		new_rpad = clamp(new_rpad, 0, H - PAD_H);

		unsigned int now = sys_ticks();
		if (now - last < STEP_TICKS)
			continue;
		last = now;

		/* Redraw player paddle if moved. */
		if (new_rpad != rpad) {
			clear_rect(W - MARGIN - PAD_W, rpad, PAD_W, PAD_H);
			rpad = new_rpad;
			fill_rect(W - MARGIN - PAD_W, rpad, PAD_W, PAD_H, PAD_C);
		}

		/* AI tracks ball centre, moving at most PAD_SPEED/2 per step. */
		int ball_cy = by + BALL_SZ / 2;
		int pad_cy  = lpad + PAD_H / 2;
		int ai_move = ball_cy - pad_cy;
		if (ai_move >  PAD_SPEED / 2) ai_move =  PAD_SPEED / 2;
		if (ai_move < -PAD_SPEED / 2) ai_move = -PAD_SPEED / 2;
		int new_lpad = clamp(lpad + ai_move, 0, H - PAD_H);
		if (new_lpad != lpad) {
			clear_rect(MARGIN, lpad, PAD_W, PAD_H);
			lpad = new_lpad;
			fill_rect(MARGIN, lpad, PAD_W, PAD_H, AI_C);
		}

		/* Move ball. */
		clear_rect(bx, by, BALL_SZ, BALL_SZ);
		bx += vx;
		by += vy;

		/* Top / bottom wall bounce. */
		if (by <= 0)           { by = 0;            vy = -vy; }
		if (by >= H - BALL_SZ) { by = H - BALL_SZ;  vy = -vy; }

		/* Left paddle collision. */
		if (vx < 0 &&
		    bx <= MARGIN + PAD_W &&
		    bx + BALL_SZ >= MARGIN &&
		    by + BALL_SZ >= lpad &&
		    by <= lpad + PAD_H) {
			bx = MARGIN + PAD_W;
			vx = -vx;
			/* Deflect vy based on hit position relative to paddle centre. */
			int rel = (by + BALL_SZ / 2) - (lpad + PAD_H / 2);
			vy = (rel * SPEED_Y * 2) / PAD_H;
			if (vy == 0) vy = 1;
			sys_tone(600, 2);
		}

		/* Right paddle collision. */
		int rpx = W - MARGIN - PAD_W;
		if (vx > 0 &&
		    bx + BALL_SZ >= rpx &&
		    bx <= rpx + PAD_W &&
		    by + BALL_SZ >= rpad &&
		    by <= rpad + PAD_H) {
			bx = rpx - BALL_SZ;
			vx = -vx;
			int rel = (by + BALL_SZ / 2) - (rpad + PAD_H / 2);
			vy = (rel * SPEED_Y * 2) / PAD_H;
			if (vy == 0) vy = 1;
			sys_tone(800, 2);
		}

		/* Scoring: ball left screen. */
		if (bx + BALL_SZ < 0) {
			rscore++;
			draw_score(lscore, rscore);
			sys_tone(400, 8);
			if (rscore >= WIN_SCORE) {
				sys_log("pong: player wins!");
				sys_drawtext(W / 2 - 48, H / 2 - 4, "PLAYER WINS!", FG);
				sys_tone(1000, 15);
				return 1;
			}
			bx = W / 2 - BALL_SZ / 2;
			by = H / 2 - BALL_SZ / 2;
			vx = SPEED_X; vy = SPEED_Y;
		}
		if (bx > W) {
			lscore++;
			draw_score(lscore, rscore);
			sys_tone(300, 8);
			if (lscore >= WIN_SCORE) {
				sys_log("pong: AI wins!");
				sys_drawtext(W / 2 - 36, H / 2 - 4, "AI WINS!", FG);
				sys_tone(200, 15);
				return 1;
			}
			bx = W / 2 - BALL_SZ / 2;
			by = H / 2 - BALL_SZ / 2;
			vx = -SPEED_X; vy = SPEED_Y;
		}

		fill_rect(bx, by, BALL_SZ, BALL_SZ, BALL_C);
	}
}

void main(void)
{
	for (;;) {
		int again = play_round();
		if (!again) {
			sys_log("pong: quit");
			return;
		}
		sys_log("pong: r=play again  q=quit");
		for (;;) {
			int c = sys_getkey();
			if (c == 'q') return;
			if (c == 'r') break;
		}
	}
}
