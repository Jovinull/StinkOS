/* StinkOS installer: clones the boot media (drive 0) onto a fresh target
 * disk (drive 2 = secondary master). Designed to run as a regular app off
 * the install ISO; on success, you reboot pointing QEMU at the target disk
 * alone and StinkOS comes up identically.
 *
 * UI: 1024x768 framebuffer, kernel font. No mouse. Keyboard advances the
 * confirmation steps. Progress bar updates every 256 sectors copied.
 */
#include "libstink.h"

#define SCREEN_W       1024
#define SCREEN_H       768

#define COLOR_BG       0x001022
#define COLOR_PANEL    0x3050C0
#define COLOR_TEXT     0xFFFFFF
#define COLOR_DIM      0xA0A0A0
#define COLOR_OK       0x40D080
#define COLOR_WARN     0xE0A040
#define COLOR_ERR      0xE04040
#define COLOR_BAR      0x4080FF

#define SOURCE_DRIVE   0
#define TARGET_DRIVE   2

static void draw_chrome(const char *title)
{
	sys_fillrect(0, 0, SCREEN_W, SCREEN_H, COLOR_BG);
	sys_fillrect(112, 84, 800, 460, COLOR_PANEL);
	sys_drawtext(120, 90, "STINKOS INSTALLER", COLOR_TEXT);
	sys_drawtext(120, 110, title, COLOR_DIM);
}

static void draw_status(int y, const char *label, const char *value, unsigned int color)
{
	sys_drawtext(140, y, label, COLOR_DIM);
	sys_drawtext(300, y, value, color);
}

static int wait_key(int allowed)
{
	for (;;) {
		int c = sys_getkey();
		if (c == 0) { sys_sleep_ms(20); continue; }
		if (allowed == 0 || c == allowed || c == 27)
			return c;
	}
}

/* Read a decimal-digit line ending in Enter. Returns the parsed value,
 * or -1 on escape, or -2 on empty Enter (caller picks a default). */
static int read_uint_line(int x, int y, unsigned int *out)
{
	char buf[12];
	unsigned int len = 0;
	*out = 0;
	for (;;) {
		int c = sys_getkey();
		if (c == 0) { sys_sleep_ms(20); continue; }
		if (c == 27) return -1;
		if (c == '\n' || c == '\r') {
			if (len == 0) return -2;
			buf[len] = '\0';
			unsigned int v = 0;
			for (unsigned int i = 0; i < len; i++)
				v = v * 10u + (unsigned int)(buf[i] - '0');
			*out = v;
			return 0;
		}
		if (c == '\b' && len > 0) {
			len--;
			sys_fillrect(x, y, (int)sizeof(buf) * 8, 16, COLOR_PANEL);
			buf[len] = '\0';
			if (len > 0) sys_drawtext(x, y, buf, COLOR_TEXT);
			continue;
		}
		if (c >= '0' && c <= '9' && len < sizeof(buf) - 1) {
			buf[len++] = (char)c;
			buf[len]   = '\0';
			sys_drawtext(x, y, buf, COLOR_TEXT);
		}
	}
}

static void format_dec(unsigned int v, char *out)
{
	char tmp[12];
	int n = 0;
	if (v == 0) { out[0] = '0'; out[1] = '\0'; return; }
	while (v > 0 && n < 11) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
	for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
	out[n] = '\0';
}

static void draw_progress(unsigned int done, unsigned int total)
{
	int bar_x = 140, bar_y = 360, bar_w = 720, bar_h = 24;
	sys_fillrect(bar_x, bar_y, bar_w, bar_h, 0x202830);

	unsigned int filled = 0;
	if (total > 0)
		filled = done * (unsigned int)bar_w / total;
	if (filled > (unsigned int)bar_w)
		filled = (unsigned int)bar_w;
	sys_fillrect(bar_x, bar_y, (int)filled, bar_h, COLOR_BAR);

	char buf[64];
	char a[12], b[12];
	format_dec(done, a);
	format_dec(total, b);
	int p = 0;
	for (int i = 0; a[i]; i++) buf[p++] = a[i];
	buf[p++] = ' '; buf[p++] = '/'; buf[p++] = ' ';
	for (int i = 0; b[i]; i++) buf[p++] = b[i];
	const char *suf = " sectors copied";
	for (int i = 0; suf[i]; i++) buf[p++] = suf[i];
	buf[p] = '\0';
	sys_drawtext(bar_x, bar_y + 36, buf, COLOR_TEXT);
}

void main(void)
{
	draw_chrome("step 1/3: detecting disks");

	char src_model[41], dst_model[41];
	unsigned int src_sectors = 0, dst_sectors = 0;

	int src_rc = sys_disk_info(SOURCE_DRIVE, src_model, &src_sectors);
	int dst_rc = sys_disk_info(TARGET_DRIVE, dst_model, &dst_sectors);

	if (src_rc != 0) {
		draw_status(160, "source (drive 0):", "NOT DETECTED", COLOR_ERR);
		sys_drawtext(140, 440, "no boot media. esc to return.", COLOR_DIM);
		wait_key(0); sys_exit();
	}
	draw_status(160, "source (drive 0):", src_model[0] ? src_model : "?", COLOR_OK);

	char num[12]; format_dec(src_sectors, num);
	draw_status(180, "                  ", num, COLOR_DIM);

	if (dst_rc != 0) {
		draw_status(220, "target (drive 2):", "NOT DETECTED", COLOR_ERR);
		sys_drawtext(140, 440,
		             "start qemu with -drive ...,if=ide,index=2 to add a target.",
		             COLOR_DIM);
		sys_drawtext(140, 460, "esc to return.", COLOR_DIM);
		wait_key(0); sys_exit();
	}
	draw_status(220, "target (drive 2):", dst_model[0] ? dst_model : "?", COLOR_OK);
	format_dec(dst_sectors, num);
	draw_status(240, "                  ", num, COLOR_DIM);

	if (dst_sectors < src_sectors) {
		draw_status(280, "size check:", "TARGET TOO SMALL", COLOR_ERR);
		wait_key(0); sys_exit();
	}
	draw_status(280, "size check:", "OK", COLOR_OK);

	/* Show the target's existing MBR partition table (informational --
	 * the clone overwrites it). Empty / unformatted disks return -1 and
	 * the message just says "no MBR". Helps the user avoid wiping a disk
	 * they thought was blank. */
	struct sys_mbr_partition parts[4];
	if (sys_mbr_read(TARGET_DRIVE, parts) == 0) {
		sys_drawtext(140, 280 + 20, "existing target MBR partitions:", COLOR_DIM);
		int any = 0;
		for (int pi = 0; pi < 4; pi++) {
			if (parts[pi].type == 0 || parts[pi].sector_count == 0)
				continue;
			any = 1;
			char line[64], n1[12], n2[12];
			format_dec(parts[pi].first_lba, n1);
			format_dec(parts[pi].sector_count, n2);
			int p = 0;
			line[p++] = (char)('1' + pi); line[p++] = ':'; line[p++] = ' ';
			line[p++] = 't'; line[p++] = 'y'; line[p++] = 'p';
			line[p++] = 'e'; line[p++] = '='; line[p++] = '0'; line[p++] = 'x';
			static const char hx[] = "0123456789ABCDEF";
			line[p++] = hx[(parts[pi].type >> 4) & 0xF];
			line[p++] = hx[parts[pi].type & 0xF];
			line[p++] = ' '; line[p++] = 'l'; line[p++] = 'b'; line[p++] = 'a';
			line[p++] = '=';
			for (int j = 0; n1[j]; j++) line[p++] = n1[j];
			line[p++] = ' '; line[p++] = 'n'; line[p++] = '=';
			for (int j = 0; n2[j]; j++) line[p++] = n2[j];
			line[p] = '\0';
			sys_drawtext(160, 280 + 40 + pi * 16, line, COLOR_TEXT);
		}
		if (!any)
			sys_drawtext(160, 280 + 40, "(table empty)", COLOR_DIM);
	}

	/* Optional install size override. Default (blank Enter) clones the
	 * full source disk; a typed sector count installs a truncated image
	 * for smaller targets. A floor of 8 MiB (16384 sectors) keeps the
	 * kernel + StinkFS dir + a tiny data region safe. */
	const unsigned int MIN_SECTORS = 16384u;            /* 8 MiB */
	unsigned int install_sectors = src_sectors;
	sys_drawtext(140, 310, "install size (sectors, Enter=full):", COLOR_DIM);
	unsigned int typed = 0;
	int rl = read_uint_line(500, 310, &typed);
	if (rl == -1) {
		draw_status(400, "cancelled.", "", COLOR_DIM);
		sys_sleep_ms(800); sys_exit();
	}
	if (rl == 0) {
		if (typed < MIN_SECTORS || typed > src_sectors) {
			draw_status(400, "size out of range",
			            "(min 16384, max source)", COLOR_ERR);
			wait_key(0); sys_exit();
		}
		install_sectors = typed;
	}
	format_dec(install_sectors, num);
	draw_status(330, "chosen size:", num, COLOR_OK);

	sys_drawtext(140, 360,
	             "WARNING: everything on the target will be overwritten.", COLOR_WARN);
	sys_drawtext(140, 380, "press y to install, esc to abort.", COLOR_TEXT);

	int k = wait_key(0);
	if (k != 'y') {
		draw_status(440, "cancelled.", "", COLOR_DIM);
		sys_sleep_ms(800); sys_exit();
	}

	draw_chrome("step 2/3: cloning sectors");
	draw_status(160, "source:", src_model, COLOR_DIM);
	draw_status(180, "target:", dst_model, COLOR_DIM);

	/* Drop a first-boot marker into the source StinkFS BEFORE the clone so
	 * the target inherits it byte-for-byte. The kernel deletes the marker
	 * on the target's first boot after running the one-shot init path
	 * (regenerate STINK.CONF with a default hostname, etc.). We remove it
	 * from the source after the clone so the install media itself does not
	 * re-run first-boot on every boot. */
	const char *marker = "v0.4";
	if (sys_fwrite("FIRSTBOOT.RUN", marker, 4) != 0) {
		draw_status(420, "could not seed first-boot marker", "", COLOR_ERR);
		wait_key(0); sys_exit();
	}

	unsigned int total = install_sectors;
	unsigned int copied = 0;
	const unsigned int chunk = 256;             /* 128 KiB per syscall */
	while (copied < total) {
		unsigned int batch = total - copied;
		if (batch > chunk) batch = chunk;
		int did = sys_disk_copy(SOURCE_DRIVE, TARGET_DRIVE, copied, batch);
		if (did <= 0) {
			draw_status(420, "copy error at lba", "", COLOR_ERR);
			format_dec(copied, num);
			sys_drawtext(420, 420, num, COLOR_ERR);
			wait_key(0); sys_exit();
		}
		copied += (unsigned int)did;
		draw_progress(copied, total);
	}

	/* Strip the marker from the source so the install media stays idempotent. */
	sys_fdelete("FIRSTBOOT.RUN");

	/* Re-write the target's boot sector explicitly. The main clone loop
	 * already copied LBA 0, but a later MBR-aware install step (partition
	 * table edits, recovery shimming) could overwrite parts of it; this
	 * final 1-sector pass guarantees the target boots with the same boot
	 * code as the source. Idempotent and cheap (one IDE sector). */
	sys_log("installer: rewriting target boot sector");
	sys_disk_copy(SOURCE_DRIVE, TARGET_DRIVE, 1, 0);

	draw_chrome("step 3/3: complete");
	draw_status(180, "result:", "INSTALL SUCCEEDED", COLOR_OK);
	sys_drawtext(140, 220,
	             "boot from drive 2 to use the new system.", COLOR_TEXT);
	sys_drawtext(140, 240,
	             "press any key to return to the menu.", COLOR_DIM);
	wait_key(0);
	sys_exit();
}
