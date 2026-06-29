/* StinkOS userland app: Asteroids.
 * Classic vector game using sys_drawline and libstink_math.
 */
#include "libstink.h"

#define BG 0x001022
#define FG 0xFFFFFF

/* Math constants */
#define M_PI 3.14159265358979323846

#define SCREEN_W 1024
#define SCREEN_H  768

#define MAX_BULLETS 10
#define MAX_ASTEROIDS 20

struct Ship {
    double x, y;
    double vx, vy;
    double angle;
    int active;
};

struct Bullet {
    int active;
    double x, y;
    double vx, vy;
    int life;
};

struct Asteroid {
    int active;
    double x, y;
    double vx, vy;
    int size; /* 3=large, 2=med, 1=small */
    double angle;
    double rot_speed;
};

static struct Ship ship;
static struct Bullet bullets[MAX_BULLETS];
static struct Asteroid asteroids[MAX_ASTEROIDS];
static int score = 0;
static int game_over = 0;
static int wave = 1;

/* Simple PRNG since we don't have rand() */
static unsigned int lcg_seed = 12345;
static unsigned int next_rand(void) {
    lcg_seed = lcg_seed * 1103515245 + 12345;
    return (unsigned int)(lcg_seed / 65536) % 32768;
}
static double rand_double(void) {
    return (double)next_rand() / 32767.0;
}

static void clear_screen(void)
{
    sys_fillrect(0, 0, 1024, 768, BG);
}

static void spawn_asteroid(double x, double y, int size)
{
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!asteroids[i].active) {
            asteroids[i].active = 1;
            asteroids[i].x = x;
            asteroids[i].y = y;
            double ang = rand_double() * M_PI * 2.0;
            double speed = ((4 - size) * 0.5 + rand_double() * 0.5) + (wave * 0.2);
            asteroids[i].vx = cos(ang) * speed;
            asteroids[i].vy = sin(ang) * speed;
            asteroids[i].size = size;
            asteroids[i].angle = 0;
            asteroids[i].rot_speed = (rand_double() - 0.5) * 0.2;
            break;
        }
    }
}

static void spawn_wave(int count)
{
    for (int i = 0; i < count; i++) {
        /* Spawn at edges */
        double ax = rand_double() > 0.5 ? 0 : 1024;
        double ay = rand_double() * 768;
        spawn_asteroid(ax, ay, 3);
    }
}

static void init_game(void)
{
    ship.x = 1024.0 / 2.0;
    ship.y = 768.0 / 2.0;
    ship.vx = 0;
    ship.vy = 0;
    ship.angle = -M_PI / 2.0; /* pointing up */
    ship.active = 1;

    for (int i = 0; i < MAX_BULLETS; i++) bullets[i].active = 0;
    for (int i = 0; i < MAX_ASTEROIDS; i++) asteroids[i].active = 0;

    score = 0;
    game_over = 0;
    wave = 1;
    lcg_seed = sys_ticks();

    spawn_wave(4);
}

static void draw_ship(void)
{
    if (!ship.active) return;
    double scale = 15.0;
    
    double p1_ang = ship.angle;
    double p2_ang = ship.angle + (M_PI * 0.8);
    double p3_ang = ship.angle - (M_PI * 0.8);
    
    int x1 = (int)(ship.x + cos(p1_ang) * scale);
    int y1 = (int)(ship.y + sin(p1_ang) * scale);
    
    int x2 = (int)(ship.x + cos(p2_ang) * scale);
    int y2 = (int)(ship.y + sin(p2_ang) * scale);
    
    int x3 = (int)(ship.x + cos(p3_ang) * scale);
    int y3 = (int)(ship.y + sin(p3_ang) * scale);
    
    sys_drawline(x1, y1, x2, y2, FG);
    sys_drawline(x2, y2, x3, y3, FG);
    sys_drawline(x3, y3, x1, y1, FG);
}

static void draw_asteroid(struct Asteroid *a)
{
    if (!a->active) return;
    
    int pts = 8;
    double scale = a->size * 12.0;
    int px[8], py[8];
    
    for (int i = 0; i < pts; i++) {
        double ang = a->angle + (i * (M_PI * 2.0 / pts));
        /* slight irregularity could be added here */
        double rad = scale;
        px[i] = (int)(a->x + cos(ang) * rad);
        py[i] = (int)(a->y + sin(ang) * rad);
    }
    
    for (int i = 0; i < pts; i++) {
        sys_drawline(px[i], py[i], px[(i+1)%pts], py[(i+1)%pts], FG);
    }
}

static void draw_bullet(struct Bullet *b)
{
    if (!b->active) return;
    int bx = (int)b->x;
    int by = (int)b->y;
    if (bx < 0) bx = 0;
    if (bx > SCREEN_W - 2) bx = SCREEN_W - 2;
    if (by < 0) by = 0;
    if (by > SCREEN_H - 2) by = SCREEN_H - 2;
    sys_fillrect(bx, by, 2, 2, FG);
}

static void wrap(double *x, double *y)
{
    if (*x < 0) *x += 1024;
    if (*x >= 1024) *x -= 1024;
    if (*y < 0) *y += 768;
    if (*y >= 768) *y -= 768;
}

static int check_collision(double x1, double y1, double x2, double y2, double r)
{
    double dx = x1 - x2;
    double dy = y1 - y2;
    return (dx*dx + dy*dy) < (r*r);
}

void main(void)
{
    init_game();
    unsigned int last_tick = sys_ticks();
    sys_log("asteroids: starting up");

    for (;;) {
        /* Process all queued keys */
        int c;
        while ((c = sys_getkey()) != 0) {
            if (c == 'q') {
                return;
            }
            if (c == 'r' && game_over) {
                init_game();
            }
            if (c == ' ' && ship.active) {
                /* Shoot */
                for (int i = 0; i < MAX_BULLETS; i++) {
                    if (!bullets[i].active) {
                        bullets[i].active = 1;
                        bullets[i].x = ship.x + cos(ship.angle) * 15.0;
                        bullets[i].y = ship.y + sin(ship.angle) * 15.0;
                        bullets[i].vx = ship.vx + cos(ship.angle) * 10.0;
                        bullets[i].vy = ship.vy + sin(ship.angle) * 10.0;
                        bullets[i].life = 40;
                        break;
                    }
                }
            }
            if (ship.active) {
                if (c == KEY_LEFT) ship.angle -= 0.3;
                if (c == KEY_RIGHT) ship.angle += 0.3;
                if (c == KEY_UP) {
                    ship.vx += cos(ship.angle) * 2.0;
                    ship.vy += sin(ship.angle) * 2.0;
                }
            }
        }

        /* Framerate control (~100fps) */
        unsigned int now = sys_ticks();
        if (now == last_tick) {
            sys_sleep_ms(2);
            continue;
        }
        last_tick = now;

        /* Physics */
        if (ship.active) {
            ship.x += ship.vx;
            ship.y += ship.vy;
            ship.vx *= 0.99; /* friction */
            ship.vy *= 0.99;
            wrap(&ship.x, &ship.y);
        }

        for (int i = 0; i < MAX_BULLETS; i++) {
            if (bullets[i].active) {
                bullets[i].x += bullets[i].vx;
                bullets[i].y += bullets[i].vy;
                wrap(&bullets[i].x, &bullets[i].y);
                bullets[i].life--;
                if (bullets[i].life <= 0) bullets[i].active = 0;
            }
        }

        int active_asts = 0;
        for (int i = 0; i < MAX_ASTEROIDS; i++) {
            if (asteroids[i].active) {
                active_asts++;
                asteroids[i].x += asteroids[i].vx;
                asteroids[i].y += asteroids[i].vy;
                asteroids[i].angle += asteroids[i].rot_speed;
                wrap(&asteroids[i].x, &asteroids[i].y);
                
                double r = asteroids[i].size * 12.0;

                /* Check collision with bullets */
                for (int b = 0; b < MAX_BULLETS; b++) {
                    if (bullets[b].active && check_collision(asteroids[i].x, asteroids[i].y, bullets[b].x, bullets[b].y, r)) {
                        bullets[b].active = 0;
                        asteroids[i].active = 0;
                        score += 100 * (4 - asteroids[i].size);
                        if (asteroids[i].size > 1) {
                            spawn_asteroid(asteroids[i].x, asteroids[i].y, asteroids[i].size - 1);
                            spawn_asteroid(asteroids[i].x, asteroids[i].y, asteroids[i].size - 1);
                        }
                        break;
                    }
                }
                
                /* Check collision with ship */
                if (ship.active && asteroids[i].active && check_collision(asteroids[i].x, asteroids[i].y, ship.x, ship.y, r + 10.0)) {
                    ship.active = 0;
                    game_over = 1;
                }
            }
        }
        
        /* Next wave */
        if (active_asts == 0 && !game_over) {
            wave++;
            spawn_wave(4 + wave);
        }

        /* Render */
        clear_screen();
        if (game_over) {
            /* Simple visual indicator: ship disappears, maybe a red square */
            int ex = (int)ship.x - 10;
            int ey = (int)ship.y - 10;
            if (ex < 0) ex = 0;
            if (ey < 0) ey = 0;
            if (ex > SCREEN_W - 20) ex = SCREEN_W - 20;
            if (ey > SCREEN_H - 20) ey = SCREEN_H - 20;
            sys_fillrect(ex, ey, 20, 20, 0xFF0000);
        } else {
            draw_ship();
        }
        for (int i = 0; i < MAX_BULLETS; i++) draw_bullet(&bullets[i]);
        for (int i = 0; i < MAX_ASTEROIDS; i++) draw_asteroid(&asteroids[i]);
    }
}
