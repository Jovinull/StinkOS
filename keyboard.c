/* PS/2 keyboard driver: reads scancode set 1 from the i8042 data port (0x60),
 * tracks shift state, and reports decoded characters on the serial console. */
#include "keyboard.h"
#include "serial.h"
#include "io.h"

#define KBD_DATA   0x60

#define SC_LSHIFT  0x2A
#define SC_RSHIFT  0x36
#define SC_RELEASE 0x80          /* bit 7 set => key release */
#define SC_EXTENDED 0xE0         /* prefix byte: the next code is "extended"
                                  * (arrows, Home/End, the numpad's siblings) */

#define SC_ARROW_UP    0x48
#define SC_ARROW_DOWN  0x50
#define SC_ARROW_LEFT  0x4B
#define SC_ARROW_RIGHT 0x4D

/* Scancode set 1 -> ASCII (unshifted). 0 means "no printable char". */
static const char map_normal[128] = {
	0,    27,  '1', '2', '3', '4', '5', '6', '7', '8',   /* 0x00-0x09 */
	'9', '0', '-', '=', '\b','\t','q', 'w', 'e', 'r',     /* 0x0A-0x13 */
	't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,      /* 0x14-0x1D */
	'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',     /* 0x1E-0x27 */
	'\'','`', 0,  '\\','z', 'x', 'c', 'v', 'b', 'n',       /* 0x28-0x31 */
	'm', ',', '.', '/', 0,  '*', 0,  ' ', 0,   0,         /* 0x32-0x3B */
};

/* Shifted variants for the same scancodes. */
static const char map_shift[128] = {
	0,    27,  '!', '@', '#', '$', '%', '^', '&', '*',
	'(', ')', '_', '+', '\b','\t','Q', 'W', 'E', 'R',
	'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
	'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
	'"', '~', 0,  '|', 'Z', 'X', 'C', 'V', 'B', 'N',
	'M', '<', '>', '?', 0,  '*', 0,  ' ', 0,   0,
};

static int shift_down = 0;
static int extended_prefix = 0;   /* set after seeing SC_EXTENDED, for one byte */

/* Decoded-character ring buffer, drained by keyboard_getchar (the syscall). */
#define KBUF_SIZE 128
static volatile char kbuf[KBUF_SIZE];
static volatile int  kbuf_head;
static volatile int  kbuf_tail;

static void kbuf_push(char c)
{
	int next = (kbuf_head + 1) % KBUF_SIZE;
	if (next != kbuf_tail) {
		kbuf[kbuf_head] = c;
		kbuf_head = next;
	}
}

char keyboard_getchar(void)
{
	if (kbuf_head == kbuf_tail)
		return 0;                          /* empty */
	char c = kbuf[kbuf_tail];
	kbuf_tail = (kbuf_tail + 1) % KBUF_SIZE;
	return c;
}

void keyboard_init(void)
{
	/* drain a possibly-pending byte so the first real key raises IRQ1 */
	inb(KBD_DATA);
}

void keyboard_handle(void)
{
	unsigned char sc = inb(KBD_DATA);

	if (sc == SC_EXTENDED) {
		extended_prefix = 1;
		return;
	}

	if (extended_prefix) {
		extended_prefix = 0;
		if (sc & SC_RELEASE)
			return;                 /* only act on the key going down */

		char arrow;
		switch (sc) {
		case SC_ARROW_UP:    arrow = KEY_UP;    break;
		case SC_ARROW_DOWN:  arrow = KEY_DOWN;  break;
		case SC_ARROW_LEFT:  arrow = KEY_LEFT;  break;
		case SC_ARROW_RIGHT: arrow = KEY_RIGHT; break;
		default:             return;            /* other extended key: ignore */
		}
		serial_write("kbd: arrow\n");
		kbuf_push(arrow);
		return;
	}

	if (sc & SC_RELEASE) {
		unsigned char code = sc & 0x7F;
		if (code == SC_LSHIFT || code == SC_RSHIFT)
			shift_down = 0;
		return;
	}

	if (sc == SC_LSHIFT || sc == SC_RSHIFT) {
		shift_down = 1;
		return;
	}

	char c = shift_down ? map_shift[sc] : map_normal[sc];
	if (c == 0)
		return;

	serial_write("kbd: ");
	serial_putc(c);
	serial_putc('\n');

	kbuf_push(c);                              /* make it available to userland */
}
