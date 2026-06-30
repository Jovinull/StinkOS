/* StinkOS userland app: single-player Pong. UP/DOWN arrows (or W/S) control
 * the right paddle; the left paddle is AI. First to 7 points wins. Score
 * logged to serial. 'q' quits to menu at any time. */
#include "libstink.h"

#define W           1024
#define H           768
#define OY          34                  /* titlebar height + separator */
#define GH          (H - OY)           /* game field height */
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

/* All drawing goes through these wrappers so game-space Y is offset by OY. */
static void gfill(int x, int y, int w, int h, unsigned int rgb)
{
	sys_fillrect(x, y + OY, w, h, rgb);
}

static void gclear(int x, int y, int w, int h)
{
	sys_fillrect(x, y + OY, w, h, BG);
}

static void draw_score(int lscore, int rscore)
{
	sys_printf("pong: %d - %d", lscore, rscore);

	/* Render "L - R" centred in the game area near the top. */
	char buf[6];
	buf[0] = '0' + (lscore % 10);
	buf[1] = ' '; buf[2] = '-'; buf[3] = ' ';
	buf[4] = '0' + (rscore % 10);
	buf[5] = '\0';
	/* 5 glyphs x 8px = 40px wide; centre at W/2 = 512 -> x = 492 */
	gfill(480, 6, 64, 12, BG);         /* erase old score */
	sys_drawtext(492, OY + 6, buf, FG);
}

static int pclamp(int v, int lo, int hi)
{
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

static int play_round(void)   /* returns 0 = quit, 1 = play again */
{
	int bx = W / 2 - BALL_SZ / 2;
	int by = GH / 2 - BALL_SZ / 2;
	int vx = SPEED_X, vy = SPEED_Y;

	int lpad = GH / 2 - PAD_H / 2;   /* AI pad y (game-space) */
	int rpad = GH / 2 - PAD_H / 2;   /* player pad y */

	int lscore = 0, rscore = 0;

	/* Draw window frame, then game area. */
	draw_window_frame(0, 0, W, H, "Pong  --  UP/DOWN or W/S to move  |  q to quit");
	gfill(0, 0, W, GH, BG);

	/* Centre dashed line (game-space). */
	for (int y = 0; y < GH; y += 16)
		gfill(W / 2 - 1, y, 2, 8, 0x334466);

	gfill(MARGIN, lpad, PAD_W, PAD_H, AI_C);
	gfill(W - MARGIN - PAD_W, rpad, PAD_W, PAD_H, PAD_C);
	gfill(bx, by, BALL_SZ, BALL_SZ, BALL_C);

	sys_log("pong: running (arrows/WS=paddle, q=quit)");
	unsigned int last = sys_ticks();

	for (;;) {
		int c = sys_getkey();
		if (c == 'q') return 0;

		/* Player paddle. */
		int new_rpad = rpad;
		if ((c == KEY_UP || c == 'w') && rpad > 0)
			new_rpad -= PAD_SPEED;
		if ((c == KEY_DOWN || c == 's') && rpad < GH - PAD_H)
			new_rpad += PAD_SPEED;
		new_rpad = pclamp(new_rpad, 0, GH - PAD_H);

		unsigned int now = sys_ticks();
		if (now - last < STEP_TICKS)
			continue;
		last = now;

		/* Redraw player paddle if moved. */
		if (new_rpad != rpad) {
			gclear(W - MARGIN - PAD_W, rpad, PAD_W, PAD_H);
			rpad = new_rpad;
			gfill(W - MARGIN - PAD_W, rpad, PAD_W, PAD_H, PAD_C);
		}

		/* AI tracks ball centre, moving at most PAD_SPEED/2 per step. */
		int ball_cy = by + BALL_SZ / 2;
		int pad_cy  = lpad + PAD_H / 2;
		int ai_move = ball_cy - pad_cy;
		if (ai_move >  PAD_SPEED / 2) ai_move =  PAD_SPEED / 2;
		if (ai_move < -PAD_SPEED / 2) ai_move = -PAD_SPEED / 2;
		int new_lpad = pclamp(lpad + ai_move, 0, GH - PAD_H);
		if (new_lpad != lpad) {
			gclear(MARGIN, lpad, PAD_W, PAD_H);
			lpad = new_lpad;
			gfill(MARGIN, lpad, PAD_W, PAD_H, AI_C);
		}

		/* Move ball. */
		gclear(bx, by, BALL_SZ, BALL_SZ);
		bx += vx;
		by += vy;

		/* Top / bottom wall bounce (game-space). */
		if (by <= 0)             { by = 0;              vy = -vy; }
		if (by >= GH - BALL_SZ)  { by = GH - BALL_SZ;  vy = -vy; }

		/* Left paddle collision. */
		if (vx < 0 &&
		    bx <= MARGIN + PAD_W &&
		    bx + BALL_SZ >= MARGIN &&
		    by + BALL_SZ >= lpad &&
		    by <= lpad + PAD_H) {
			bx = MARGIN + PAD_W;
			vx = -vx;
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
				sys_drawtext(W / 2 - 48, OY + GH / 2 - 4, "PLAYER WINS!", FG);
				sys_tone(1000, 15);
				return 1;
			}
			bx = W / 2 - BALL_SZ / 2;
			by = GH / 2 - BALL_SZ / 2;
			vx = SPEED_X; vy = SPEED_Y;
		}
		if (bx > W) {
			lscore++;
			draw_score(lscore, rscore);
			sys_tone(300, 8);
			if (lscore >= WIN_SCORE) {
				sys_log("pong: AI wins!");
				sys_drawtext(W / 2 - 36, OY + GH / 2 - 4, "AI WINS!", FG);
				sys_tone(200, 15);
				return 1;
			}
			bx = W / 2 - BALL_SZ / 2;
			by = GH / 2 - BALL_SZ / 2;
			vx = -SPEED_X; vy = SPEED_Y;
		}

		gfill(bx, by, BALL_SZ, BALL_SZ, BALL_C);
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
