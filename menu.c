/* Start menu: draws a list of the apps stored on raw disk slots, lets the user
 * move a cursor with 'w'/'s' and launch the selection with Enter. Launching a
 * program loads it from its slot and drops into ring 3 (does not return). */
#include "menu.h"
#include "fb.h"
#include "keyboard.h"
#include "serial.h"
#include "ata.h"
#include "paging.h"

extern void enter_user_mode(unsigned int entry, unsigned int user_stack);

struct kctx { unsigned int esp, ebp, ebx, esi, edi, eip; };
extern int  ksetjmp(struct kctx *ctx);
extern void klongjmp(struct kctx *ctx, int val);

static struct kctx exit_ctx;     /* where to resume when an app calls SYS_EXIT */

#define APP_ADDR     0x400000
#define APP_SECTORS  4

struct app_entry {
	const char  *name;
	unsigned int lba;
};

static const struct app_entry apps[] = {
	{ "1 HELLO", 64 },
	{ "2 BOX",   72 },
};
#define APP_COUNT (int)(sizeof(apps) / sizeof(apps[0]))

static unsigned char app_stack[4096] __attribute__((aligned(16)));

static void draw(int selected)
{
	fb_fill(0x001022);
	fb_rect(112, 84, 800, 400, 0x3050C0);
	fb_text(120, 90, "STINKOS", 0xFFFFFF);

	for (int i = 0; i < APP_COUNT; i++) {
		fb_text(140, 120 + i * 20, apps[i].name, 0xFFFFFF);
		if (i == selected)
			fb_text(124, 120 + i * 20, ">", 0xFFFF00);
	}
}

void menu_exit(void)
{
	klongjmp(&exit_ctx, 1);                 /* jump back into launch() */
}

static void launch(int index)
{
	if (ksetjmp(&exit_ctx) != 0)
		return;                         /* app called SYS_EXIT: back to menu */

	ata_read(apps[index].lba, APP_SECTORS, (void *)APP_ADDR);
	serial_write("loader: app loaded from disk slot\n");
	paging_set_user(APP_ADDR);
	paging_set_user((unsigned int)app_stack);
	enter_user_mode(APP_ADDR, (unsigned int)(app_stack + sizeof(app_stack)));
}

void menu_run(void)
{
	int selected = 0;

	draw(selected);
	serial_write("menu: ready\n");

	for (;;) {
		char c = keyboard_getchar();
		if (c == 0) {
			__asm__ volatile ("hlt");
			continue;
		}
		if (c == 's' && selected < APP_COUNT - 1) {
			selected++;
			draw(selected);
		} else if (c == 'w' && selected > 0) {
			selected--;
			draw(selected);
		} else if (c == '\n') {
			launch(selected);          /* returns here when the app exits */
			__asm__ volatile ("sti");  /* longjmp skipped the syscall's iret */
			serial_write("menu: back\n");
			draw(selected);
		}
	}
}
