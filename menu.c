/* Start menu: draws a list of the apps stored on raw disk slots, lets the user
 * move a cursor with 'w'/'s' and launch the selection with Enter. Launching a
 * program loads it from its slot and drops into ring 3 (does not return). */
#include "menu.h"
#include "fb.h"
#include "keyboard.h"
#include "serial.h"
#include "ata.h"
#include "paging.h"
#include "fs.h"

extern void enter_user_mode(unsigned int entry, unsigned int user_stack);

struct kctx { unsigned int esp, ebp, ebx, esi, edi, eip; };
extern int  ksetjmp(struct kctx *ctx);
extern void klongjmp(struct kctx *ctx, int val);

static struct kctx exit_ctx;     /* where to resume when an app calls SYS_EXIT */

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
}

void menu_exit(void)
{
	klongjmp(&exit_ctx, 1);                 /* jump back into launch() */
}

static void launch(int index)
{
	if (ksetjmp(&exit_ctx) != 0)
		return;                         /* app called SYS_EXIT: back to menu */

	paging_reset_user_heap();
	ata_read(fs_lba(index), fs_sectors(index), (void *)paging_user_code());
	serial_write("loader: app loaded from disk slot\n");
	enter_user_mode(paging_user_code(), paging_user_stack_top());
}

void menu_run(void)
{
	int selected = 0;

	fs_init();
	draw(selected);
	serial_write("menu: ready\n");

	for (;;) {
		char c = keyboard_getchar();
		if (c == 0) {
			__asm__ volatile ("hlt");
			continue;
		}
		if (c == 's' && selected < fs_count() - 1) {
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
