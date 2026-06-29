/* StinkOS userland app: Asteroids.
 * Classic vector game using sys_drawline and libstink_math.
 */
#include "libstink.h"

#define BG 0x001022
#define FG 0xFFFFFF

/* Math constants */
#define M_PI 3.14159265358979323846

/* Game entities */
struct Ship {
    double x, y;
    double vx, vy;
    double angle;
};

static struct Ship ship;

static void clear_screen(void)
{
    sys_fillrect(0, 0, 1024, 768, BG);
}

static void draw_ship(void)
{
    /* Ship is a triangle pointing towards `angle` */
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

void main(void)
{
    ship.x = 1024.0 / 2.0;
    ship.y = 768.0 / 2.0;
    ship.vx = 0;
    ship.vy = 0;
    ship.angle = -M_PI / 2.0; /* pointing up */

    unsigned int last_tick = sys_ticks();

    sys_log("asteroids: starting up");

    for (;;) {
        int c = sys_getkey();
        
        if (c == 'q') {
            break;
        }
        
        if (c == KEY_LEFT) {
            ship.angle -= 0.1;
        } else if (c == KEY_RIGHT) {
            ship.angle += 0.1;
        } else if (c == KEY_UP) {
            ship.vx += cos(ship.angle) * 0.5;
            ship.vy += sin(ship.angle) * 0.5;
        }

        /* Wait for next tick to control framerate (approx 100fps) */
        unsigned int now = sys_ticks();
        if (now == last_tick) {
            sys_sleep_ms(10);
            continue;
        }
        last_tick = now;

        /* Physics update */
        ship.x += ship.vx;
        ship.y += ship.vy;
        
        /* Friction */
        ship.vx *= 0.99;
        ship.vy *= 0.99;
        
        /* Screen wrap */
        if (ship.x < 0) ship.x += 1024;
        if (ship.x >= 1024) ship.x -= 1024;
        if (ship.y < 0) ship.y += 768;
        if (ship.y >= 768) ship.y -= 768;

        /* Render */
        clear_screen();
        draw_ship();
    }
}
