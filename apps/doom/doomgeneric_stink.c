/* doomgeneric platform layer for StinkOS.
 *
 * The kernel exposes a 1024x768 framebuffer; doomgeneric renders into a
 * 640x400 ARGB buffer (DG_ScreenBuffer, allocated by doomgeneric.c). DG_Draw-
 * Frame just blits the Doom buffer at a centred position via SYS_BLIT --
 * one kernel call per frame, not 256K. The unused borders are painted black
 * once during DG_Init.
 *
 * Input arrives as raw PS/2 scancode events from SYS_GETKEYEVENT (press AND
 * release). convertToDoomKey() maps each interesting set-1 scancode to the
 * Doom keycode the upper layers expect; the rest of the table is bypassed,
 * so spurious keys are ignored rather than causing fake input. */

#include "doomkeys.h"
#include "doomgeneric.h"

#include "libstink.h"

#define DOOM_X_OFFSET ((1024 - DOOMGENERIC_RESX) / 2)
#define DOOM_Y_OFFSET ((768  - DOOMGENERIC_RESY) / 2)

/* Translated Doom key events, queued between sys_get_keyevent polls and the
 * DG_GetKey calls from doomgeneric. Each entry is (pressed << 8) | doomKey. */
#define KEY_QUEUE_CAP 64
static unsigned short key_queue[KEY_QUEUE_CAP];
static unsigned int   key_queue_w;
static unsigned int   key_queue_r;

/* Maps a PS/2 scancode-set-1 byte to the Doom key the rest of the engine
 * uses. 0 means "no mapping" -- the event is dropped silently. */
static unsigned char convert_to_doom_key(unsigned char sc, int extended)
{
	if (extended) {
		switch (sc) {
		case 0x4B: return KEY_LEFTARROW;
		case 0x4D: return KEY_RIGHTARROW;
		case 0x48: return KEY_UPARROW;
		case 0x50: return KEY_DOWNARROW;
		case 0x1C: return KEY_ENTER;       /* keypad Enter */
		case 0x1D: return KEY_RCTRL;       /* right Ctrl  */
		case 0x38: return KEY_RALT;        /* right Alt   */
		case 0x47: return KEY_HOME;
		case 0x4F: return KEY_END;
		case 0x49: return KEY_PGUP;
		case 0x51: return KEY_PGDN;
		case 0x52: return KEY_INS;
		case 0x53: return KEY_DEL;
		default:   return 0;
		}
	}

	switch (sc) {
	case 0x01: return KEY_ESCAPE;
	case 0x1C: return KEY_ENTER;
	case 0x0F: return KEY_TAB;
	case 0x0E: return KEY_BACKSPACE;
	case 0x1D: return KEY_FIRE;            /* left Ctrl  -> fire */
	case 0x39: return KEY_USE;             /* space      -> use  */
	case 0x2A: return KEY_RSHIFT;          /* left Shift -> run  */
	case 0x36: return KEY_RSHIFT;          /* right Shift        */

	/* Movement letters: WASD doubles up as the cursor cluster, which is what
	 * Chocolate Doom expects when keyboard-only ("alwaysrun" feels right). */
	case 0x11: return KEY_UPARROW;         /* W */
	case 0x1F: return KEY_DOWNARROW;       /* S */
	case 0x1E: return KEY_STRAFE_L;        /* A */
	case 0x20: return KEY_STRAFE_R;        /* D */

	/* Function keys (menu shortcuts). */
	case 0x3B: return KEY_F1;
	case 0x3C: return KEY_F2;
	case 0x3D: return KEY_F3;
	case 0x3E: return KEY_F4;
	case 0x3F: return KEY_F5;
	case 0x40: return KEY_F6;
	case 0x41: return KEY_F7;
	case 0x42: return KEY_F8;
	case 0x43: return KEY_F9;
	case 0x44: return KEY_F10;

	/* Number row -- weapon select (1..7). */
	case 0x02: return '1';
	case 0x03: return '2';
	case 0x04: return '3';
	case 0x05: return '4';
	case 0x06: return '5';
	case 0x07: return '6';
	case 0x08: return '7';
	case 0x09: return '8';
	case 0x0A: return '9';
	case 0x0B: return '0';
	case 0x0C: return KEY_MINUS;
	case 0x0D: return KEY_EQUALS;

	/* "y" for the "are you sure?" prompts. */
	case 0x15: return 'y';
	case 0x31: return 'n';

	default:   return 0;
	}
}

static void key_queue_push(int pressed, unsigned char doom_key)
{
	if (doom_key == 0)
		return;
	unsigned int next = (key_queue_w + 1) % KEY_QUEUE_CAP;
	if (next == key_queue_r)
		return;                            /* queue full: drop */
	key_queue[key_queue_w] = (unsigned short)((pressed ? 0x100 : 0) | doom_key);
	key_queue_w = next;
}

static void drain_keyboard(void)
{
	unsigned int ev;
	while ((ev = sys_get_keyevent()) != 0) {
		unsigned char sc       = ev & KEY_EV_SC_MASK;
		int           pressed  = !!(ev & KEY_EV_PRESSED);
		int           extended = !!(ev & KEY_EV_EXTENDED);
		key_queue_push(pressed, convert_to_doom_key(sc, extended));
	}
}

void DG_Init(void)
{
	/* Paint the surrounding letterbox black once; the Doom rect is repainted
	 * every frame so the border is the only thing the kernel-side sys_fillrect
	 * needs to set up. */
	sys_fillrect(0, 0, 1024, 768, 0x000000);
}

void DG_DrawFrame(void)
{
	drain_keyboard();
	sys_blit(DOOM_X_OFFSET, DOOM_Y_OFFSET,
	         DOOMGENERIC_RESX, DOOMGENERIC_RESY,
	         DG_ScreenBuffer);
}

void DG_SleepMs(uint32_t ms)
{
	sys_sleep_ms(ms);
}

uint32_t DG_GetTicksMs(void)
{
	return sys_ticks() * 10u;              /* PIT ticks are 10 ms each */
}

int DG_GetKey(int *pressed, unsigned char *doomKey)
{
	if (key_queue_r == key_queue_w)
		return 0;
	unsigned short ev = key_queue[key_queue_r];
	key_queue_r = (key_queue_r + 1) % KEY_QUEUE_CAP;
	*pressed = (ev >> 8) & 1;
	*doomKey = (unsigned char)(ev & 0xFF);
	return 1;
}

void DG_SetWindowTitle(const char *title)
{
	(void)title;                           /* no window manager to tell */
}

/* doomgeneric_Create / doomgeneric_Tick form the regular game loop; argv is
 * passed straight through so -iwad and friends can be picked up. Doom's d_iwad
 * search path won't find anything on this OS, so we pre-set -iwad here -- the
 * WAD must live in StinkFS under DOOM1.WAD before launch. */
int main(int argc, char **argv)
{
	static char  arg0[] = "doom";
	static char  arg1[] = "-iwad";
	static char  arg2[] = "DOOM1.WAD";
	static char *fake_argv[3] = { arg0, arg1, arg2 };

	(void)argc;
	(void)argv;

	doomgeneric_Create(3, fake_argv);

	for (;;)
		doomgeneric_Tick();
	return 0;                              /* never reached */
}
