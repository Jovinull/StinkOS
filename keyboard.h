/* PS/2 keyboard driver (IRQ1, scancode set 1). */
#ifndef KEYBOARD_H
#define KEYBOARD_H

/* Arrow keys have no ASCII representation; report them as the otherwise
 * unused C0 control codes 1-4 so callers can tell them apart from 0
 * ("no key") and from any printable character. */
#define KEY_UP    1
#define KEY_DOWN  2
#define KEY_LEFT  3
#define KEY_RIGHT 4

void keyboard_init(void);     /* flush any pending byte from the controller */
void keyboard_handle(void);   /* called from the IRQ1 handler */
char keyboard_getchar(void);  /* dequeue a decoded char, or 0 if none */

#endif
