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
	case BOOT_OK:   return "ok";
	case BOOT_FAIL: return "FAIL";
	default:        return "absent";
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
	/* ── Background ─────────────────────────────────────────────────────── */
	fb_fill(0x0d1117);   /* dark GitHub-style bg */

	/* ── Logo: "StinkOS" at 4x scale ────────────────────────────────────── */
	unsigned int logo_x = 128;
	unsigned int logo_y =  80;
	fb_text_big(logo_x, logo_y, "StinkOS", 0x57f287, 4);   /* accent green */

	/* Accent underline bar */
	fb_rect(logo_x, logo_y + 36, 7 * 8 * 4, 3, 0x57f287);

	/* Tagline */
	fb_text(logo_x + 2, logo_y + 48, "an original x86 32-bit hobby OS", 0x8b949e);

	/* ── POST table (two columns) ────────────────────────────────────────── */
	unsigned int col1_x = 160;
	unsigned int col2_x = 500;
	unsigned int row_y  = logo_y + 76;
	unsigned int row_h  = 16;

	/* Column headers */
	fb_text(col1_x, row_y - 18, "component", 0x30363d);
	fb_text(col2_x, row_y - 18, "status",    0x30363d);
	fb_rect(col1_x, row_y - 4, 640, 1, 0x21262d);

	int any_fail = 0;
	for (int i = 0; i < entry_count; i++) {
		unsigned int y = row_y + (unsigned int)i * row_h;
		unsigned int col;
		switch (entries[i].status) {
		case BOOT_OK:   col = 0x57f287; break;
		case BOOT_FAIL: col = 0xf47067; any_fail = 1; break;
		default:        col = 0x4a5568; break;   /* BOOT_ABSENT: dim */
		}
		fb_text(col1_x, y, entries[i].name, 0xe6edf3);
		fb_text(col2_x, y, status_text(entries[i].status), col);
	}

	/* Summary line */
	unsigned int sum_y = row_y + (unsigned int)entry_count * row_h + 8;
	fb_rect(col1_x, sum_y - 4, 640, 1, 0x21262d);
	if (any_fail)
		fb_text(col1_x, sum_y, "boot completed with errors", 0xf47067);
	else
		fb_text(col1_x, sum_y, "all systems nominal", 0x57f287);

	/* ── Hold ~0.8 s so the panel is readable ───────────────────────────── */
	unsigned int start = pit_ticks();
	while ((unsigned int)(pit_ticks() - start) < 80)
		__asm__ volatile ("hlt");
}
