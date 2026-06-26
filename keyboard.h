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
/* Navigation keys: stored as negative chars (high bytes) so they never collide
 * with printable ASCII (32-126), Ctrl codes (1-26), arrow codes (28-31), or
 * the zero sentinel that marks an empty kbuf. */
#define KEY_HOME  (-6)
#define KEY_END   (-5)
#define KEY_PGUP  (-4)
#define KEY_PGDN  (-3)

void keyboard_init(void);     /* flush any pending byte from the controller */
void keyboard_handle(void);   /* called from the IRQ1 handler */
char keyboard_getchar(void);  /* dequeue a decoded char, or 0 if none */

/* Raw key-event queue: every scancode byte (press AND release) is reported,
 * regardless of whether it produces a decoded char. Returns 0 when empty,
 * otherwise a 32-bit packed event with bit 31 set as a present-marker:
 *   bit 15    : pressed (1) / released (0)
 *   bit 8     : extended prefix saw (1) / regular scancode (0)
 *   bits 7..0 : raw scancode byte with the release bit stripped
 * Doom and similar apps consume these (via SYS_GETKEYEVENT) to track key
 * state instead of just typed characters. */
unsigned int keyboard_get_event(void);

#endif
