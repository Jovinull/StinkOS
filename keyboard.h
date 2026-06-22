/* PS/2 keyboard driver (IRQ1, scancode set 1). */
#ifndef KEYBOARD_H
#define KEYBOARD_H

void keyboard_init(void);     /* flush any pending byte from the controller */
void keyboard_handle(void);   /* called from the IRQ1 handler */
char keyboard_getchar(void);  /* dequeue a decoded char, or 0 if none */

#endif
