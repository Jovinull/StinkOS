/* PS/2 keyboard driver (IRQ1, scancode set 1). */
#ifndef KEYBOARD_H
#define KEYBOARD_H

/* Arrow keys have no ASCII representation; report them as otherwise-unused
 * C0 control codes so callers can tell them apart from 0 ("no key"), from
 * any printable character, and from a Ctrl+letter combo (which now reports
 * the standard ASCII control code 1-26, e.g. Ctrl+C -> 3). 28-31 (FS/GS/RS/US)
 * are never produced by Ctrl+letter and have no other use in this driver. */
#define KEY_UP    28
#define KEY_DOWN  29
#define KEY_LEFT  30
#define KEY_RIGHT 31

void keyboard_init(void);     /* flush any pending byte from the controller */
void keyboard_handle(void);   /* called from the IRQ1 handler */
char keyboard_getchar(void);  /* dequeue a decoded char, or 0 if none */

#endif
