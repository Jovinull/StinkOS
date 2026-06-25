/* Start menu: draws a list of the apps stored on raw disk slots, lets the user
 * move a cursor with 'w'/'s' and launch the selection with Enter. Launching a
 * program loads it from its slot and drops into ring 3 (does not return). */
#include "menu.h"
#include "fb.h"
#include "keyboard.h"
#include "mouse.h"
#include "serial.h"
#include "ata.h"
#include "paging.h"
#include "elf.h"
#include "fs.h"

#define APP_IMAGE_MAX 16384            /* staging buffer for an app's ELF image */

extern void enter_user_mode(unsigned int entry, unsigned int user_stack);

struct kctx { unsigned int esp, ebp, ebx, esi, edi, eip; };
extern int  ksetjmp(struct kctx *ctx);
extern void klongjmp(struct kctx *ctx, int val);

static struct kctx exit_ctx;     /* where to resume when an app calls SYS_EXIT */
static unsigned char app_image[APP_IMAGE_MAX];   /* ELF image staging buffer */

/* Clickable area for the "press f for files" / "press q to return" line. */
#define FILES_BTN_X 112
#define FILES_BTN_Y 436
#define FILES_BTN_W 300
#define FILES_BTN_H 16

/* Clickable area for menu item i: a horizontal band over its text + arrow. */
#define ITEM_X 120
#define ITEM_Y(i) (120 + (i) * 20 - 2)
#define ITEM_W 760
#define ITEM_H 20

enum { VIEW_MENU = 0, VIEW_FILES = 1 };
static int view = VIEW_MENU;
static int selected;
static int files_selected;

/* Mouse state from the previous frame, used to detect motion (so the cursor
 * is only redrawn when it actually moves) and left-button edge (a "click" is
 * the transition from released to pressed, not the held-down state). */
static int last_mouse_x = -1, last_mouse_y = -1;
static unsigned char last_mouse_buttons;

static int point_in_box(int x, int y, int bx, int by, int bw, int bh)
{
	return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

static void draw(int selected)
{
	fb_fill(0x001022);
	fb_rect(112, 84, 800, 400, 0x3050C0);
	fb_text(120, 90, "STINKOS", 0xFFFFFF);

	for (int i = 0; i < fs_count(); i++) {
		fb_text(140, 120 + i * 20, fs_name(i), 0xFFFFFF);
		if (i == selected)
			fb_text(124, 120 + i * 20, ">", 0xFFFF00);
	}

	fb_text(FILES_BTN_X + 8, FILES_BTN_Y + 4, "press f for files", 0xA0A0A0);
}

/* Decimal text for a file size; buf must hold at least 11 bytes. */
static void size_to_text(unsigned int v, char *buf)
{
	char tmp[10];
	int n = 0;

	if (v == 0) {
		buf[0] = '0';
		buf[1] = '\0';
		return;
	}
	while (v > 0 && n < 10) {
		tmp[n++] = '0' + (v % 10);
		v /= 10;
	}
	for (int i = 0; i < n; i++)
		buf[i] = tmp[n - 1 - i];
	buf[n] = '\0';
}

static void draw_files(void)
{
	fb_fill(0x001022);
	fb_rect(112, 84, 800, 400, 0x3050C0);
	fb_text(120, 90, "FILES", 0xFFFFFF);

	int count = fs_file_count();
	for (int i = 0; i < count; i++) {
		char name[16];
		int size = fs_file_info(i, name);
		if (size < 0)
			continue;
		name[15] = '\0';

		char line[32];
		int p = 0;
		for (int j = 0; name[j] != '\0' && p < 20; j++)
			line[p++] = name[j];
		line[p++] = ' ';
		char num[11];
		size_to_text((unsigned int)size, num);
		for (int j = 0; num[j] != '\0' && p < 31; j++)
			line[p++] = num[j];
		line[p] = '\0';

		fb_text(140, 120 + i * 20, line, 0xFFFFFF);
		if (i == files_selected)
			fb_text(124, 120 + i * 20, ">", 0xFFFF00);
	}

	if (count == 0)
		fb_text(140, 120, "no files", 0xA0A0A0);

	fb_text(FILES_BTN_X + 8, FILES_BTN_Y + 4, "q: return   d: delete selected", 0xA0A0A0);
}

/* Repaints whichever screen is current, then the mouse cursor on top of it.
 * Since there is no back buffer, "erasing" the old cursor position just
 * means repainting the whole screen before drawing the cursor at the new
 * spot -- the same approach the rest of the menu already uses on every
 * keypress, so this adds no new flicker behaviour. */
static void redraw(void)
{
	if (view == VIEW_FILES)
		draw_files();
	else
		draw(selected);
	mouse_draw_cursor(0xFFFF00);
}

void menu_exit(void)
{
	klongjmp(&exit_ctx, 1);                 /* jump back into launch() */
}

static void launch(int index)
{
	if (ksetjmp(&exit_ctx) != 0)
		return;                         /* app called SYS_EXIT: back to menu */

	unsigned int sectors = fs_sectors(index);
	if (sectors * 512 > APP_IMAGE_MAX) {
		serial_write("loader: app image too large\n");
		return;
	}

	paging_reset_user_heap();
	if (ata_read(fs_lba(index), sectors, app_image) != 0) {
		serial_write("loader: disk read failed\n");
		return;
	}

	unsigned int entry;
	if (elf_load(app_image, sectors * 512, &entry) != 0) {
		serial_write("loader: bad ELF image\n");
		return;
	}
	serial_write("loader: app loaded from disk slot\n");
	enter_user_mode(entry, paging_user_stack_top());
}

void menu_run(void)
{
	selected = 0;
	view = VIEW_MENU;

	fs_init();
	mouse_get_state(&last_mouse_x, &last_mouse_y, &last_mouse_buttons);
	redraw();
	serial_write("menu: ready\n");

	for (;;) {
		char c = keyboard_getchar();

		int mx, my;
		unsigned char mb;
		mouse_get_state(&mx, &my, &mb);
		int moved = (mx != last_mouse_x || my != last_mouse_y);
		/* A "click" is the release->press edge, not the held-down level,
		 * so dragging across the menu doesn't fire one action per frame. */
		int clicked = (mb & MOUSE_LEFT_BTN) && !(last_mouse_buttons & MOUSE_LEFT_BTN);
		last_mouse_x = mx;
		last_mouse_y = my;
		last_mouse_buttons = mb;

		if (c == 0 && !moved && !clicked) {
			__asm__ volatile ("hlt");
			continue;
		}

		if (view == VIEW_FILES) {
			int count = fs_file_count();

			if (c == 'q' || (clicked && point_in_box(mx, my, FILES_BTN_X, FILES_BTN_Y, FILES_BTN_W, FILES_BTN_H))) {
				view = VIEW_MENU;
				redraw();
			} else if (c == 's' && files_selected < count - 1) {
				files_selected++;
				redraw();
			} else if (c == 'w' && files_selected > 0) {
				files_selected--;
				redraw();
			} else if (c == 'd' && files_selected < count) {
				char name[16];
				if (fs_file_info(files_selected, name) >= 0) {
					name[15] = '\0';
					fs_file_delete(name);
					serial_write("menu: deleted file\n");
				}
				if (files_selected >= fs_file_count() && files_selected > 0)
					files_selected--;
				redraw();
			} else if (clicked) {
				int hit = -1;
				for (int i = 0; i < count; i++)
					if (point_in_box(mx, my, ITEM_X, ITEM_Y(i), ITEM_W, ITEM_H))
						hit = i;
				if (hit >= 0) {
					files_selected = hit;
					redraw();
				}
			} else if (moved) {
				/* Only the cursor changed: cheaper than a full repaint. */
				mouse_undraw_cursor();
				mouse_draw_cursor(0xFFFF00);
			}
			continue;
		}

		if (c == 's' && selected < fs_count() - 1) {
			selected++;
			redraw();
		} else if (c == 'w' && selected > 0) {
			selected--;
			redraw();
		} else if (c == '\n') {
			launch(selected);          /* returns here when the app exits */
			__asm__ volatile ("sti");  /* longjmp skipped the syscall's iret */
			serial_write("menu: back\n");
			redraw();
		} else if (c == 'f' || (clicked && point_in_box(mx, my, FILES_BTN_X, FILES_BTN_Y, FILES_BTN_W, FILES_BTN_H))) {
			serial_write("menu: files view\n");
			view = VIEW_FILES;
			files_selected = 0;
			redraw();
		} else if (clicked) {
			int hit = -1;
			for (int i = 0; i < fs_count(); i++)
				if (point_in_box(mx, my, ITEM_X, ITEM_Y(i), ITEM_W, ITEM_H))
					hit = i;
			if (hit >= 0) {
				selected = hit;
				redraw();
				launch(selected);
				__asm__ volatile ("sti");
				serial_write("menu: back\n");
				redraw();
			}
		} else if (moved) {
			/* Only the cursor changed: cheaper than a full repaint. */
			mouse_undraw_cursor();
			mouse_draw_cursor(0xFFFF00);
		}
	}
}
