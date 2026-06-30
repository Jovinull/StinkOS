/* StinkOS userland: Breakout. Left/Right (or A/D) moves paddle.
 * Q quits. Break all bricks to advance; 3 lives. */
#include "libstink.h"

#define W           1024
#define H           768
#define OY          34
#define GH          (H - OY)

/* Colors */
#define BG          0x000a10
#define FG          0xffffff
#define PAD_C       0x00b4d8
#define BALL_C      0xf8f9fa

/* Paddle */
#define PAD_W       100
#define PAD_H       12
#define PAD_Y       (H - OY - 28)
#define PAD_SPD     9

/* Ball */
#define BALL_SZ     10
#define BALL_VX0    4
#define BALL_VY0    (-6)

/* Brick grid */
#define BCOLS       14
#define BROWS       8
#define BW          62
#define BH          18
#define BGAP        4
#define BOFF_X      ((W - (BCOLS * (BW + BGAP) - BGAP)) / 2)
#define BOFF_Y      (OY + 48)

/* Row colors (BROWS entries) */
static const unsigned int ROW_COLORS[BROWS] = {
    0xe63946, 0xe8693f, 0xf4a261, 0xe9c46a,
    0x2a9d8f, 0x457b9d, 0x5e548e, 0x9b72cf,
};

/* ── State ──────────────────────────────────────────────────────────────── */

static unsigned char bricks[BROWS][BCOLS]; /* 1=alive, 0=dead */
static int pad_x;   /* left edge of paddle */
static int bx, by;  /* ball position (top-left) */
static int bvx, bvy;
static int score, lives, level;

static void init_bricks(void) {
    for (int r = 0; r < BROWS; r++)
        for (int c = 0; c < BCOLS; c++)
            bricks[r][c] = 1;
}

static void reset_ball(void) {
    bx  = W / 2 - BALL_SZ / 2;
    by  = OY + GH * 2 / 3;
    bvx = BALL_VX0 + level;
    bvy = BALL_VY0 - level;
}

/* ── Drawing ─────────────────────────────────────────────────────────────── */

static void draw_hud(void) {
    /* Score label */
    char buf[32];
    int i = 0;
    buf[i++]='S'; buf[i++]='c'; buf[i++]='o'; buf[i++]='r'; buf[i++]='e'; buf[i++]=':'; buf[i++]=' ';
    unsigned int s = (unsigned int)score;
    if (s == 0) { buf[i++]='0'; }
    else {
        char tmp[8]; int ti=0;
        while (s) { tmp[ti++]='0'+(s%10); s/=10; }
        /* tmp has digits in reverse order */
        int j=ti;
        while (j-->0) buf[i++]=tmp[j];
    }
    buf[i]=0;
    /* Re-draw title area */
    draw_window_frame(0, 0, W, H, buf);
}

static void draw_bricks(void) {
    for (int r = 0; r < BROWS; r++) {
        for (int c = 0; c < BCOLS; c++) {
            int bx2 = BOFF_X + c * (BW + BGAP);
            int by2 = BOFF_Y + r * (BH + BGAP);
            unsigned int col = bricks[r][c] ? ROW_COLORS[r] : BG;
            sys_fillrect(bx2, by2, BW, BH, col);
        }
    }
}

static void draw_lives(void) {
    int lx = W - 80;
    int ly = OY + 4;
    for (int i = 0; i < 3; i++) {
        unsigned int c = i < lives ? 0xff6b6b : 0x1a1a2e;
        sys_fillrect(lx + i * 18, ly, 14, 14, c);
    }
}

static void erase_ball(void) { sys_fillrect(bx, by, BALL_SZ, BALL_SZ, BG); }
static void draw_ball(void)  { sys_fillrect(bx, by, BALL_SZ, BALL_SZ, BALL_C); }
static void erase_pad(int x) { sys_fillrect(x, OY + PAD_Y, PAD_W, PAD_H, BG); }
static void draw_pad(int x)  { sys_fillrect(x, OY + PAD_Y, PAD_W, PAD_H, PAD_C); }

/* ── Collision ───────────────────────────────────────────────────────────── */

static int rects_overlap(int ax, int ay, int aw, int ah,
                          int bx2, int by2, int bw, int bh) {
    return ax < bx2+bw && ax+aw > bx2 && ay < by2+bh && ay+ah > by2;
}

/* ── Main ────────────────────────────────────────────────────────────────── */

void main(void) {
    score  = 0; lives = 3; level = 0;
    pad_x  = W / 2 - PAD_W / 2;

    sys_fillrect(0, 0, W, H, BG);
    init_bricks();
    reset_ball();
    draw_window_frame(0, 0, W, H, "Breakout");
    draw_bricks();
    draw_lives();
    draw_pad(pad_x);
    draw_ball();

    unsigned int prev_t = sys_ticks();

    while (1) {
        /* ~60 fps cap */
        unsigned int now = sys_ticks();
        if (now - prev_t < 2) {
            sys_sleep_ms(10);
            continue;
        }
        prev_t = now;

        /* Input */
        int c = sys_getkey();
        if (c == 'q' || c == 'Q') break;
        int left  = (c == KEY_LEFT  || c == 'a' || c == 'A');
        int right = (c == KEY_RIGHT || c == 'd' || c == 'D');

        /* Move paddle */
        int new_pad = pad_x;
        if (left)  new_pad -= PAD_SPD;
        if (right) new_pad += PAD_SPD;
        if (new_pad < 0)          new_pad = 0;
        if (new_pad > W - PAD_W)  new_pad = W - PAD_W;
        if (new_pad != pad_x) {
            erase_pad(pad_x);
            pad_x = new_pad;
            draw_pad(pad_x);
        }

        /* Move ball */
        erase_ball();
        bx += bvx;
        by += bvy;

        /* Wall bounces (sides + top) */
        if (bx < 0)            { bx = 0;            bvx = -bvx; }
        if (bx + BALL_SZ > W)  { bx = W - BALL_SZ;  bvx = -bvx; }
        if (by < OY)           { by = OY;            bvy = -bvy; }

        /* Paddle collision */
        if (rects_overlap(bx, by, BALL_SZ, BALL_SZ, pad_x, OY + PAD_Y, PAD_W, PAD_H)) {
            by  = OY + PAD_Y - BALL_SZ;
            bvy = -bvy;
            /* Angle based on hit position */
            int rel = (bx + BALL_SZ / 2) - (pad_x + PAD_W / 2);
            bvx = rel / 8 + (bvx > 0 ? 1 : -1);
            if (bvx == 0) bvx = 1;
        }

        /* Ball lost */
        if (by + BALL_SZ > H) {
            lives--;
            draw_lives();
            if (lives <= 0) break;
            reset_ball();
            draw_ball();
            sys_sleep_ms(800);
            continue;
        }

        /* Brick collisions */
        int hit = 0;
        for (int r = 0; r < BROWS && !hit; r++) {
            for (int c = 0; c < BCOLS && !hit; c++) {
                if (!bricks[r][c]) continue;
                int bx2 = BOFF_X + c * (BW + BGAP);
                int by2 = BOFF_Y + r * (BH + BGAP);
                if (rects_overlap(bx, by, BALL_SZ, BALL_SZ, bx2, by2, BW, BH)) {
                    bricks[r][c] = 0;
                    sys_fillrect(bx2, by2, BW, BH, BG);
                    score += 10 * (BROWS - r);
                    draw_hud();
                    /* Determine bounce axis from previous position */
                    int prev_by = by - bvy;
                    int was_above = prev_by + BALL_SZ <= by2;
                    int was_below = prev_by >= by2 + BH;
                    if (was_above || was_below) bvy = -bvy;
                    else                        bvx = -bvx;
                    hit = 1;
                    sys_sound(400 + r * 80);
                    sys_sleep_ms(10);
                    sys_sound(0);
                }
            }
        }

        /* Check cleared */
        int total = 0;
        for (int r = 0; r < BROWS; r++)
            for (int c = 0; c < BCOLS; c++)
                total += bricks[r][c];
        if (total == 0) {
            level++;
            sys_sound(880); sys_sleep_ms(200); sys_sound(0);
            init_bricks();
            reset_ball();
            sys_fillrect(0, OY, W, GH, BG);
            draw_bricks();
            draw_lives();
            draw_pad(pad_x);
        }

        draw_ball();
    }

    /* Game over screen */
    sys_fillrect(0, OY, W, GH, BG);
    draw_window_frame(0, 0, W, H, "Breakout - Game Over");
    sys_sleep_ms(1500);
}
