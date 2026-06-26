/* Boot-time POST diagnostics -- see bootdiag.h. */
#include "bootdiag.h"
#include "fb.h"
#include "serial.h"
#include "interrupts.h"

#define MAX_ENTRIES 24
#define NAME_MAX    16

static struct {
	char             name[NAME_MAX];
	enum boot_status status;
} entries[MAX_ENTRIES];
static int entry_count;

static void copy_name(char *dst, const char *src)
{
	int i = 0;
	for (; i < NAME_MAX - 1 && src[i]; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

static const char *status_text(enum boot_status s)
{
	switch (s) {
	case BOOT_OK:   return "OK";
	case BOOT_FAIL: return "FAIL";
	default:        return "ABSENT";
	}
}

void bootdiag_add(const char *name, enum boot_status status)
{
	if (entry_count < MAX_ENTRIES) {
		copy_name(entries[entry_count].name, name);
		entries[entry_count].status = status;
		entry_count++;
	}

	serial_write("post: ");
	serial_write(name);
	serial_putc(' ');
	serial_write(status_text(status));
	serial_putc('\n');
}

void bootdiag_show(void)
{
	fb_fill(0x000010);
	fb_text(120, 80, "StinkOS POST", 0xFFFFFF);
	fb_text(120, 100, "power-on self-test", 0x8080A0);

	int any_fail = 0;
	for (int i = 0; i < entry_count; i++) {
		int y = 130 + i * 16;
		unsigned int col;
		switch (entries[i].status) {
		case BOOT_OK:   col = 0x40D040; break;
		case BOOT_FAIL: col = 0xE04040; any_fail = 1; break;
		default:        col = 0x808080; break;   /* BOOT_ABSENT */
		}
		fb_text(140, y, entries[i].name, 0xC0C0C0);
		fb_text(360, y, status_text(entries[i].status), col);
	}

	int y = 130 + entry_count * 16 + 16;
	if (any_fail)
		fb_text(140, y, "boot completed with errors", 0xE04040);
	else
		fb_text(140, y, "all systems nominal", 0x40D040);

	/* Hold ~1.2 s (120 PIT ticks at 100 Hz) so the panel is readable. Sleep on
	 * hlt rather than spin so the timer IRQ keeps advancing the counter. */
	unsigned int start = pit_ticks();
	while ((unsigned int)(pit_ticks() - start) < 120)
		__asm__ volatile ("hlt");
}
