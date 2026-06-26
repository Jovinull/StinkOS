/* PS/2 mouse driver (auxiliary port of the 8042 controller, IRQ12).
 * NOT YET WIRED INTO THE KERNEL: mouse_init() must be called once after the
 * IDT/PIC are set up, and mouse_handle() must be called from the IRQ12
 * handler, both of which live in Core's interrupts.c/kernel.c. */
#ifndef MOUSE_H
#define MOUSE_H

#define MOUSE_LEFT_BTN   0x01
#define MOUSE_RIGHT_BTN  0x02
#define MOUSE_MIDDLE_BTN 0x04

void mouse_init(unsigned int screen_w, unsigned int screen_h);
void mouse_handle(unsigned char data);  /* called from the IRQ12 handler */
void mouse_get_state(int *x, int *y, unsigned char *buttons);

/* Drains the relative-motion accumulator into *dx and *dy (screen-space units:
 * +x = right, +y = down) and reports current button state. Resets the
 * accumulator to zero before returning, so back-to-back calls only see new
 * motion. Designed for apps that need raw deltas (mouselook, drawing tools)
 * instead of the clamped absolute cursor. */
void mouse_consume_delta(int *dx, int *dy, unsigned char *buttons);

/* Draws the cursor at the current position, saving whatever pixels were
 * there so mouse_undraw_cursor() can put them back. Call mouse_undraw_cursor()
 * before moving the cursor again (or before any other drawing touches the
 * same spot) to avoid leaving a visible cross behind. */
void mouse_draw_cursor(unsigned int rgb);
void mouse_undraw_cursor(void);

#endif
