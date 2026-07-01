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
#include "proc.h"
#include "elf.h"
#include "fs.h"
#include "rtc.h"
#include "audio.h"
#include "net.h"

extern void enter_user_mode(unsigned int entry, unsigned int user_stack);

struct kctx { unsigned int esp, ebp, ebx, esi, edi, eip; };
extern int  ksetjmp(struct kctx *ctx);
extern void klongjmp(struct kctx *ctx, int val);

static struct kctx exit_ctx;     /* where to resume when an app calls SYS_EXIT */

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

enum { VIEW_MENU = 0, VIEW_FILES = 1, VIEW_INFO = 2 };
static int view = VIEW_MENU;
static int selected;
static int files_selected;

/* Mouse state from the previous frame, used to detect motion (so the cursor
 * is only redrawn when it actually moves) and left-button edge (a "click" is
 * the transition from released to pressed, not the held-down state). */
static int last_mouse_x = -1, last_mouse_y = -1;
static unsigned char last_mouse_buttons;


/* Formats two-digit decimal n into buf[0..1], no NUL (caller handles it). */
static void fmt2(unsigned int n, char *buf)
{
	buf[0] = '0' + (n / 10) % 10;
	buf[1] = '0' + n % 10;
}

static void draw_clock_from(const struct rtc_time *t)
{
	/* "HH:MM:SS" — 8 chars × 8px wide, anchored to the right of the header */
	char s[9];
	fmt2(t->hour,   s + 0); s[2] = ':';
	fmt2(t->minute, s + 3); s[5] = ':';
	fmt2(t->second, s + 6); s[8] = '\0';

	/* 1024 - 8*8 - 8px margin = 944 */
	fb_text(944, 90, s, 0x80B0FF);
}

static unsigned int last_rtc_second = 0xFF;   /* sentinel: invalid second */

/* App list built from StinkFS at boot: only .ELF files shown in the menu. */
#define MAX_APP_LIST 40
static char app_names[MAX_APP_LIST][16];
static int  app_count;

static int is_elf_name(const char *name)
{
	int len = 0;
	while (len < 16 && name[len]) len++;
	if (len < 5) return 0;
	const char *sfx = ".ELF";
	for (int k = 0; k < 4; k++) {
		char c = name[len - 4 + k];
		if (c >= 'a' && c <= 'z') c -= 32;
		if (c != sfx[k]) return 0;
	}
	return 1;
}

static void load_app_list(void)
{
	app_count = 0;
	int fc = fs_file_count();
	for (int i = 0; i < fc && app_count < MAX_APP_LIST; i++) {
		char name[16];
		if (fs_file_info(i, name) < 0) continue;
		if (!is_elf_name(name)) continue;
		for (int k = 0; k < 16; k++)
			app_names[app_count][k] = name[k];
		app_count++;
	}
}

/* Copy the app display name (strip ".ELF" suffix) into out (NUL-terminated). */
static void display_name(const char *full, char *out)
{
	int len = 0;
	while (len < 16 && full[len]) len++;
	if (len >= 4) len -= 4;   /* strip ".ELF" */
	for (int i = 0; i < len; i++) out[i] = full[i];
	out[len] = '\0';
}

static int point_in_box(int x, int y, int bx, int by, int bw, int bh)
{
	return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

/* Case-insensitive compare of two NUL-terminated names. */
static int ci_eq(const char *a, const char *b)
{
	for (;; a++, b++) {
		char ca = *a, cb = *b;
		if (ca >= 'a' && ca <= 'z') ca -= 32;
		if (cb >= 'a' && cb <= 'z') cb -= 32;
		if (ca != cb) return 0;
		if (ca == 0)  return 1;
	}
}

/* The Doom engine ELFs are always built and stored, but each one opens a
 * specific IWAD at startup -- without it the app aborts. Map a Doom variant's
 * ELF name to the WAD it needs, or return 0 for a non-Doom app. */
static const char *doom_required_wad(const char *elf)
{
	if (ci_eq(elf, "DOOM1.ELF"))  return "FREEDOOM1.WAD";
	if (ci_eq(elf, "DOOM2.ELF"))  return "FREEDOOM2.WAD";
	if (ci_eq(elf, "FREEDM.ELF")) return "FREEDM.WAD";
	return 0;
}

/* True when 'elf' is a Doom variant whose IWAD is absent from StinkFS, so the
 * menu must mark it unavailable and refuse to launch it. */
static int app_wad_missing(const char *elf)
{
	const char *wad = doom_required_wad(elf);
	if (!wad) return 0;
	unsigned int lba, sectors;
	return fs_file_lba_sectors(wad, &lba, &sectors) != 0;
}

static void draw(int selected)
{
	fb_fill(0x001022);
	fb_rect(112, 84, 800, 400, 0x3050C0);
	fb_text(120, 90, "STINKOS", 0xFFFFFF);

	for (int i = 0; i < app_count; i++) {
		char dname[16];
		display_name(app_names[i], dname);
		int missing = app_wad_missing(app_names[i]);
		unsigned int col = missing ? 0x707070 : 0xFFFFFF;
		fb_text(140, 120 + i * 20, dname, col);
		if (missing) {
			int dl = 0;
			while (dname[dl]) dl++;
			fb_text(140 + dl * 8 + 8, 120 + i * 20, "(no wad)", 0x707070);
		}
		if (i == selected)
			fb_text(124, 120 + i * 20, ">", 0xFFFF00);
	}

	fb_text(FILES_BTN_X + 8, FILES_BTN_Y + 4, "f: files   i: disk info", 0xA0A0A0);

	struct rtc_time t;
	rtc_read(&t);
	last_rtc_second = t.second;
	draw_clock_from(&t);
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

static void draw_info(void)
{
	fb_fill(0x001022);
	fb_rect(112, 84, 800, 400, 0x3050C0);
	fb_text(120, 90, "DISK INFO", 0xFFFFFF);

	char model[41];
	unsigned int sectors;
	if (ata_identify(model, &sectors) != 0) {
		fb_text(140, 130, "no drive detected", 0xA0A0A0);
	} else {
		if (model[0] == '\0') {
			model[0] = '?';
			model[1] = '\0';
		}
		fb_text(140, 130, "model:", 0xA0A0A0);
		fb_text(140, 150, model, 0xFFFFFF);

		char num[11];
		size_to_text(sectors, num);
		char line[32];
		int p = 0;
		for (int j = 0; num[j] != '\0'; j++)
			line[p++] = num[j];
		const char *suffix = " sectors";
		for (int j = 0; suffix[j] != '\0'; j++)
			line[p++] = suffix[j];
		line[p] = '\0';
		fb_text(140, 180, "size:", 0xA0A0A0);
		fb_text(140, 200, line, 0xFFFFFF);

		size_to_text(sectors / 2048, num);              /* 512B sectors -> MB */
		p = 0;
		for (int j = 0; num[j] != '\0'; j++)
			line[p++] = num[j];
		const char *mb = " MB";
		for (int j = 0; mb[j] != '\0'; j++)
			line[p++] = mb[j];
		line[p] = '\0';
		fb_text(140, 220, line, 0xFFFFFF);
	}

	fb_text(FILES_BTN_X + 8, FILES_BTN_Y + 4, "q: return", 0xA0A0A0);
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
	else if (view == VIEW_INFO)
		draw_info();
	else
		draw(selected);
	mouse_draw_cursor(0xFFFF00);
}

void menu_exit(void)
{
	/* The IRQ-driven mixer reads sample pointers that live in the app's user
	 * pages. Once we longjmp out and reset the user heap (in launch()), those
	 * pages get unmapped; if a sound is still queued the next IRQ would walk
	 * freed memory. Cut every channel before tearing down. */
	audio_mix_silence_all();
	klongjmp(&exit_ctx, 1);                 /* jump back into launch() */
}

static void launch(int index)
{
	if (app_wad_missing(app_names[index])) {
		fb_text(140, 500, "missing WAD - cannot launch this app", 0xFF6060);
		serial_write("menu: refused launch, WAD missing\n");
		return;
	}

	if (ksetjmp(&exit_ctx) != 0)
		return;                         /* app called SYS_EXIT: back to menu */

	unsigned int lba, sectors;
	if (fs_file_lba_sectors(app_names[index], &lba, &sectors) != 0) {
		serial_write("loader: app not found in fs\n");
		return;
	}

	/* TODO §1 step 3: per-process pgdir (same dance as exec_run_by_elf
	 * in syscall.c -- duplicated for now, factor when fork lands). */
	struct proc  *cur       = proc_current();
	unsigned int *old_pgdir = cur ? (unsigned int *)cur->cr3 : 0;
	if (!old_pgdir)
		old_pgdir = paging_boot_pgdir();

	unsigned int *new_pgdir = paging_create_user_pgdir();
	if (!new_pgdir) {
		serial_write("loader: out of memory (pgdir alloc)\n");
		return;
	}
	if (paging_init_user_pgdir(new_pgdir) != 0) {
		paging_destroy_user_pgdir(new_pgdir);
		serial_write("loader: out of memory (user image)\n");
		return;
	}
	paging_activate(new_pgdir);

	unsigned int entry;
	if (elf_load(lba, sectors, &entry) != 0) {
		serial_write("loader: bad ELF image\n");
		paging_activate(old_pgdir);
		paging_destroy_user_pgdir(new_pgdir);
		return;
	}

	if (old_pgdir != new_pgdir)
		paging_destroy_user_pgdir(old_pgdir);
	if (cur)
		cur->cr3 = (unsigned int)new_pgdir;

	serial_write("loader: app loaded from fs\n");
	enter_user_mode(entry, paging_user_stack_top());
}

void menu_run(void)
{
	selected = 0;
	view = VIEW_MENU;

	if (fs_init() != 0)
		serial_write("menu: fs init failed (disk read error)\n");

	/* First-boot hook: the installer leaves FIRSTBOOT.RUN on the target
	 * before cloning, then strips it from the source after. The presence
	 * of that file on this disk means we are the freshly installed copy
	 * and have never run before. Lay down sane defaults (a generic
	 * hostname, anything else first-boot-only should plug in here) and
	 * delete the marker so subsequent boots take the fast path. */
	if (fs_file_size("FIRSTBOOT.RUN") >= 0) {
		serial_write("first-boot: initialising fresh install\n");
		const char *seed = "hostname=stinkos-fresh\n";
		unsigned int n = 0;
		while (seed[n]) n++;
		fs_file_write("STINK.CONF", seed, n);
		fs_file_delete("FIRSTBOOT.RUN");
	}

	load_app_list();

	/* Auto-launch the graphical desktop; fall back to text menu if it exits. */
	for (int i = 0; i < app_count; i++) {
		const char *want = "RS-DESKTOP";
		int j;
		for (j = 0; want[j] && app_names[i][j] == want[j]; j++) {}
		if (!want[j]) { launch(i); __asm__ volatile ("sti"); break; }
	}

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

		/* Refresh clock once per second without a full repaint. Reading
		 * rtc_read() here is cheap (fast path: chip not updating → returns
		 * immediately), and comparing seconds is the natural 1 Hz heartbeat. */
		int clock_tick = 0;
		if (view == VIEW_MENU) {
			struct rtc_time t;
			rtc_read(&t);
			if (t.second != last_rtc_second) {
				last_rtc_second = t.second;
				mouse_undraw_cursor();
				draw_clock_from(&t);
				mouse_draw_cursor(0xFFFF00);
				clock_tick = 1;
			}
		}

		if (c == 0 && !moved && !clicked && !clock_tick) {
			/* Drain at most one network frame per idle cycle so DHCP
			 * completes and any background TCP/UDP work makes progress
			 * without needing a dedicated kernel thread. */
			(void)net_poll_once();
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

		if (view == VIEW_INFO) {
			if (c == 'q' || (clicked && point_in_box(mx, my, FILES_BTN_X, FILES_BTN_Y, FILES_BTN_W, FILES_BTN_H))) {
				view = VIEW_MENU;
				redraw();
			} else if (moved) {
				mouse_undraw_cursor();
				mouse_draw_cursor(0xFFFF00);
			}
			continue;
		}

		if (c == 's' && selected < app_count - 1) {
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
		} else if (c == 'i') {
			serial_write("menu: info view\n");
			view = VIEW_INFO;
			redraw();
		} else if (clicked) {
			int hit = -1;
			for (int i = 0; i < app_count; i++)
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
