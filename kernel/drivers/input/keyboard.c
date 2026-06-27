/* PS/2 keyboard driver: reads scancode set 1 from the i8042 data port (0x60),
 * tracks shift state, and exposes two parallel paths to userland:
 *   - a decoded-character ring buffer (keyboard_getchar), which is what the
 *     shell, menu and most simple apps already consume; and
 *   - a raw key-event queue (keyboard_get_event), which reports BOTH press and
 *     release for every scancode -- the model that Doom and other key-state-
 *     driven apps need ("walking" must continue until W is released, not stop
 *     after one tap). */
#include "keyboard.h"
#include "serial.h"
#include "io.h"

#define KBD_DATA   0x60

#define SC_LSHIFT  0x2A
#define SC_RSHIFT  0x36
#define SC_CAPSLOCK 0x3A
#define SC_CTRL    0x1D          /* left Ctrl unprefixed; right Ctrl is E0 1D */
#define SC_RELEASE 0x80          /* bit 7 set => key release */
#define SC_EXTENDED 0xE0         /* prefix byte: the next code is "extended"
                                  * (arrows, Home/End, the numpad's siblings) */

#define SC_ARROW_UP    0x48
#define SC_ARROW_DOWN  0x50
#define SC_ARROW_LEFT  0x4B
#define SC_ARROW_RIGHT 0x4D
#define SC_HOME        0x47
#define SC_END         0x4F
#define SC_PGUP        0x49
#define SC_PGDN        0x51

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

/* Brazilian ABNT2 best-effort layout. Differences from US that the kernel
 * can honour without UTF-8 support:
 *   ;  ->  c  (the cedilla key, no diacritic so plain ASCII fallback)
 *   :  ->  C
 *   /  ->  ;  (the Brazilian semicolon key sits where US has the slash)
 *   ?  ->  :
 *   '  ->  ~  (no dead-key behaviour yet)
 *   "  ->  ^  (no dead-key)
 * Anything else falls through to the US table. */
static char map_br_normal[128];
static char map_br_shift [128];

/* Initialised at first switch-in -- copies US, then patches the BR diffs. */
static int br_initialised;

static void init_br_layout(void)
{
	if (br_initialised) return;
	for (int i = 0; i < 128; i++) {
		map_br_normal[i] = map_normal[i];
		map_br_shift [i] = map_shift [i];
	}
	map_br_normal[0x27] = 'c';   /* ; -> c (cedilla pos) */
	map_br_shift [0x27] = 'C';
	map_br_normal[0x35] = ';';   /* / -> ; */
	map_br_shift [0x35] = ':';
	map_br_normal[0x28] = '~';   /* ' -> ~ (no dead key) */
	map_br_shift [0x28] = '^';
	br_initialised = 1;
}

static int current_layout = KEYMAP_US;

int keyboard_set_layout(int layout)
{
	int prev = current_layout;
	if (layout == KEYMAP_BR && !br_initialised)
		init_br_layout();
	if (layout == KEYMAP_US || layout == KEYMAP_BR)
		current_layout = layout;
	return prev;
}

static const char *active_map_normal(void)
{
	return current_layout == KEYMAP_BR ? map_br_normal : map_normal;
}

static const char *active_map_shift(void)
{
	return current_layout == KEYMAP_BR ? map_br_shift : map_shift;
}

static int shift_down = 0;
static int capslock_on = 0;
static int ctrl_down = 0;
static int extended_prefix = 0;   /* set after seeing SC_EXTENDED, for one byte */

/* Decoded-character ring buffer, drained by keyboard_getchar (the syscall). */
#define KBUF_SIZE 128
static volatile char kbuf[KBUF_SIZE];
static volatile int  kbuf_head;
static volatile int  kbuf_tail;

/* Raw key-event ring buffer. Each entry packs:
 *   bit 15    : pressed (1) / released (0)
 *   bit 8     : extended prefix saw (1) / regular scancode (0)
 *   bits 7..0 : raw scancode byte with the release bit stripped
 * keyboard_get_event() then re-packs this with bit 31 set so userland can
 * distinguish "got an event" from "queue empty = 0". */
#define KEVENT_BUF_SIZE 64
static volatile unsigned short kevent_buf[KEVENT_BUF_SIZE];
static volatile int            kevent_head;
static volatile int            kevent_tail;

static void kbuf_push(char c)
{
	int next = (kbuf_head + 1) % KBUF_SIZE;
	if (next != kbuf_tail) {
		kbuf[kbuf_head] = c;
		kbuf_head = next;
	}
}

static void kevent_push(int pressed, int extended, unsigned char sc7)
{
	int next = (kevent_head + 1) % KEVENT_BUF_SIZE;
	if (next != kevent_tail) {
		kevent_buf[kevent_head] = (unsigned short)(
		    ((pressed ? 1 : 0) << 15) |
		    ((extended ? 1 : 0) << 8) |
		    (sc7 & 0x7F));
		kevent_head = next;
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

unsigned int keyboard_get_event(void)
{
	if (kevent_head == kevent_tail)
		return 0;                          /* empty */
	unsigned short ev = kevent_buf[kevent_tail];
	kevent_tail = (kevent_tail + 1) % KEVENT_BUF_SIZE;
	/* Bit 31 marks "event present"; the rest of the bits are the raw packed
	 * fields described above. Userland sees 0 for empty, non-zero on data. */
	return 0x80000000u | (unsigned int)ev;
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

	/* Snapshot before the decoded-char path below clears it. The raw event
	 * queue records (extended, scancode, pressed) for every scancode byte. */
	int is_extended = extended_prefix;
	kevent_push(!(sc & SC_RELEASE), is_extended, sc & 0x7F);

	if (is_extended) {
		extended_prefix = 0;

		if ((sc & 0x7F) == SC_CTRL) {     /* E0 1D = right Ctrl */
			ctrl_down = !(sc & SC_RELEASE);
			return;
		}
		if (sc & SC_RELEASE)
			return;                 /* only act on the key going down */

		char nav;
		switch (sc) {
		case SC_ARROW_UP:    nav = KEY_UP;    break;
		case SC_ARROW_DOWN:  nav = KEY_DOWN;  break;
		case SC_ARROW_LEFT:  nav = KEY_LEFT;  break;
		case SC_ARROW_RIGHT: nav = KEY_RIGHT; break;
		case SC_HOME:        nav = KEY_HOME;  break;
		case SC_END:         nav = KEY_END;   break;
		case SC_PGUP:        nav = KEY_PGUP;  break;
		case SC_PGDN:        nav = KEY_PGDN;  break;
		default:             return;          /* other extended key: ignore */
		}
		serial_write("kbd: nav\n");
		kbuf_push(nav);
		return;
	}

	if (sc & SC_RELEASE) {
		unsigned char code = sc & 0x7F;
		if (code == SC_LSHIFT || code == SC_RSHIFT)
			shift_down = 0;
		else if (code == SC_CTRL)
			ctrl_down = 0;
		return;
	}

	if (sc == SC_LSHIFT || sc == SC_RSHIFT) {
		shift_down = 1;
		return;
	}

	if (sc == SC_CTRL) {
		ctrl_down = 1;
		return;
	}

	if (sc == SC_CAPSLOCK) {
		capslock_on = !capslock_on;
		return;
	}

	const char *mn = active_map_normal();
	const char *ms = active_map_shift();

	/* Caps Lock only flips the case of letters, not digits/symbols -- so it
	 * has to look at whether THIS scancode maps to a letter, not just XOR
	 * the shift state unconditionally. */
	int is_letter = mn[sc] >= 'a' && mn[sc] <= 'z';
	int use_shift_map = is_letter ? (shift_down != capslock_on) : shift_down;

	char c = use_shift_map ? ms[sc] : mn[sc];
	if (c == 0)
		return;

	/* Ctrl+letter reports the standard ASCII control code (Ctrl+A=1 ...
	 * Ctrl+Z=26), same convention real terminals use -- Shift/Caps Lock
	 * don't affect it, so only the unshifted letter map matters here. */
	if (ctrl_down) {
		char base = mn[sc];
		if (base < 'a' || base > 'z')
			return;                 /* no Ctrl+digit/symbol support yet */
		c = base - 'a' + 1;
	}

	serial_write("kbd: ");
	serial_putc(c);
	serial_putc('\n');

	kbuf_push(c);                              /* make it available to userland */
}
