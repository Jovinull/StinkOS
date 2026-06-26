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

	sys_drawtext(140, 320,
	             "WARNING: everything on the target will be overwritten.", COLOR_WARN);
	sys_drawtext(140, 340, "press y to install, esc to abort.", COLOR_TEXT);

	int k = wait_key(0);
	if (k != 'y') {
		draw_status(400, "cancelled.", "", COLOR_DIM);
		sys_sleep_ms(800); sys_exit();
	}

	draw_chrome("step 2/3: cloning sectors");
	draw_status(160, "source:", src_model, COLOR_DIM);
	draw_status(180, "target:", dst_model, COLOR_DIM);

	unsigned int total = src_sectors;
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

	draw_chrome("step 3/3: complete");
	draw_status(180, "result:", "INSTALL SUCCEEDED", COLOR_OK);
	sys_drawtext(140, 220,
	             "boot from drive 2 to use the new system.", COLOR_TEXT);
	sys_drawtext(140, 240,
	             "press any key to return to the menu.", COLOR_DIM);
	wait_key(0);
	sys_exit();
}
