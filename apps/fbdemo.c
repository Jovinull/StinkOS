/* TV test-card style framebuffer demo using SYS_MAP_FB.
 * After mapping the LFB, every pixel write goes straight to VRAM with zero
 * syscall overhead -- the kernel is not involved until sys_drawtext is called
 * to overlay the title and the final sys_exit. */
#include "libstink.h"

#define W SYS_FB_W
#define H SYS_FB_H
#define S SYS_FB_STRIDE

static const unsigned int BARS[8] = {
    0xC0C0C0,   /* grey    */
    0xC0C000,   /* yellow  */
    0x00C0C0,   /* cyan    */
    0x00C000,   /* green   */
    0xC000C0,   /* magenta */
    0xC00000,   /* red     */
    0x0000C0,   /* blue    */
    0x000000,   /* black   */
};

int main(void)
{
    volatile unsigned int *fb = sys_map_fb();
    if (!fb) {
        sys_log("fbdemo: sys_map_fb failed");
        sys_exit();
    }

    unsigned int bar_h = (H * 2u) / 3u;

    /* Top two-thirds: eight EIA colour bars. */
    for (unsigned int y = 0; y < bar_h; y++) {
        for (unsigned int x = 0; x < W; x++)
            fb[y * S + x] = BARS[(x * 8u) / W];
    }

    /* Bottom third: red-to-blue gradient with a white ramp stripe. */
    for (unsigned int y = bar_h; y < H; y++) {
        for (unsigned int x = 0; x < W; x++) {
            unsigned int r = (x * 0xFFu) / (W - 1u);
            unsigned int b = 0xFFu - r;
            fb[y * S + x] = (r << 16) | b;
        }
    }

    /* Overlay a small banner using the kernel's 8x8 font renderer. */
    unsigned int mid_y = H / 2u;
    sys_drawtext(312, mid_y - 12, "DIRECT VRAM ACCESS -- SYS_MAP_FB", 0xFFFFFF);
    sys_drawtext(352, mid_y +  2, "zero syscalls per pixel written", 0xDDDDDD);
    sys_drawtext(392, mid_y + 16, "press any key to exit", 0xAAAAAA);

    sys_log("fbdemo: rendered test card via direct LFB write");

    while (!sys_getkey())
        ;
    sys_exit();
    return 0;
}
