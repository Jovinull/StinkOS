/* stink-pkg -- the StinkOS package manager front-end.
 *
 * Subcommands (selected via the boot menu's first key after launch):
 *   l - list installed packages from StinkFS
 *   i - install a package: prompt for name, GET .stinkpkg from repo,
 *       unpack files into StinkFS, register in the install database
 *   r - remove an installed package by name
 *   u - update repo index
 *   s - search repo index for a name fragment
 *
 * Today's repo URL is hardcoded; later we'll move it to a config file in
 * StinkFS. The repo serves:
 *   /index.txt        plain-text listing: "<name> <version> <sha256>"
 *   /pkg/<name>.stinkpkg
 *
 * Install DB layout in StinkFS:
 *   STINKDB         text, one line per installed pkg: "<name> <version>"
 *   pkg-<name>.lst  text, one line per file the package owns
 *
 * The actual install is sequential: parse header, iterate file entries,
 * for each entry sys_fwrite the file into StinkFS, then append to the lst.
 */
#include "libstink.h"
#include "libstink_http.h"
#include "stinkpkg.h"

#define REPO_URL_BASE   "http://stinkos-repo.local"
#define REPO_INDEX_PATH "/index.txt"
#define REPO_PKG_PATH   "/pkg/"

#define MAX_PKG_BYTES   (4u * 1024u * 1024u)   /* 4 MiB cap per package */

/* Allocated lazily in main() via malloc so the ELF stays small (the buffer
 * would otherwise live in user BSS, which is bounded by the 1 MiB code
 * window). The kernel heap goes up to ~14 MiB so 4 MiB fits comfortably. */

#define COLOR_BG     0x001022
#define COLOR_PANEL  0x3050C0
#define COLOR_TEXT   0xFFFFFF
#define COLOR_DIM    0xA0A0A0
#define COLOR_OK     0x40D080
#define COLOR_ERR    0xE04040

static unsigned char *pkg_buf;

static void draw_header(const char *title)
{
	sys_fillrect(0, 0, 1024, 768, COLOR_BG);
	sys_fillrect(112, 84, 800, 580, COLOR_PANEL);
	sys_drawtext(120, 90, "stink-pkg", COLOR_TEXT);
	sys_drawtext(120, 110, title, COLOR_DIM);
}

static void show_menu(void)
{
	draw_header("commands");
	sys_drawtext(140, 150, "l  list installed packages",       COLOR_TEXT);
	sys_drawtext(140, 170, "u  update repo index",             COLOR_TEXT);
	sys_drawtext(140, 190, "s  search repo for a name",        COLOR_TEXT);
	sys_drawtext(140, 210, "i  install a package by name",     COLOR_TEXT);
	sys_drawtext(140, 230, "r  remove an installed package",   COLOR_TEXT);
	sys_drawtext(140, 270, "esc / q  exit",                    COLOR_DIM);
}

/* Read a typed line into 'out', echoing each char at (x,y) as it comes in.
 * Returns the byte count (>= 0) on Enter, -1 on Esc. */
static int read_line(int x, int y, char *out, unsigned int cap)
{
	unsigned int len = 0;
	out[0] = '\0';
	for (;;) {
		int c = sys_getkey();
		if (c == 0) { sys_sleep_ms(20); continue; }
		if (c == 27) return -1;
		if (c == '\n' || c == '\r') return (int)len;
		if (c == '\b' && len > 0) {
			len--;
			out[len] = '\0';
			sys_fillrect(x, y, (int)cap * 8, 16, COLOR_PANEL);
			sys_drawtext(x, y, out, COLOR_TEXT);
			continue;
		}
		if (len + 1 < cap && c >= 32 && c < 127) {
			out[len++] = (char)c;
			out[len]   = '\0';
			sys_drawtext(x, y, out, COLOR_TEXT);
		}
	}
}

static void wait_any_key(int y, const char *msg, unsigned int color)
{
	sys_drawtext(140, y, msg, color);
	for (;;) {
		int c = sys_getkey();
		if (c != 0) return;
		sys_sleep_ms(20);
	}
}

/* ---- list ---- */

static void cmd_list(void)
{
	draw_header("installed packages");
	char db[2048];
	int n = sys_fread("STINKDB", db, sizeof(db) - 1);
	if (n <= 0) {
		sys_drawtext(140, 150, "no install database yet.", COLOR_DIM);
		wait_any_key(180, "press any key.", COLOR_DIM);
		return;
	}
	db[n] = '\0';

	int y = 150;
	int line_start = 0;
	for (int i = 0; i <= n; i++) {
		if (db[i] == '\n' || db[i] == '\0') {
			db[i] = '\0';
			if (i > line_start)
				sys_drawtext(140, y, db + line_start, COLOR_TEXT);
			line_start = i + 1;
			y += 18;
			if (y > 600) break;
		}
	}
	wait_any_key(620, "press any key.", COLOR_DIM);
}

/* ---- update repo index ---- */

static void cmd_update(void)
{
	draw_header("updating index");
	sys_drawtext(140, 150, "GET " REPO_URL_BASE REPO_INDEX_PATH, COLOR_DIM);

	int status = 0;
	int n = http_get(REPO_URL_BASE REPO_INDEX_PATH, pkg_buf, sizeof(pkg_buf), &status);
	if (n <= 0 || status != 200) {
		sys_drawtext(140, 190, "fetch failed.", COLOR_ERR);
		wait_any_key(220, "press any key.", COLOR_DIM);
		return;
	}
	if (sys_fwrite("REPO_INDEX", pkg_buf, (unsigned int)n) != 0) {
		sys_drawtext(140, 190, "could not save index to StinkFS.", COLOR_ERR);
		wait_any_key(220, "press any key.", COLOR_DIM);
		return;
	}
	sys_drawtext(140, 190, "index saved to REPO_INDEX.", COLOR_OK);
	char num[16]; uitoa((unsigned int)n, 10, num);
	sys_drawtext(140, 210, num, COLOR_DIM);
	sys_drawtext(220, 210, "bytes", COLOR_DIM);
	wait_any_key(240, "press any key.", COLOR_DIM);
}

/* ---- search ---- */

static void cmd_search(void)
{
	draw_header("search");
	sys_drawtext(140, 150, "name fragment:", COLOR_DIM);
	char needle[32];
	if (read_line(280, 150, needle, sizeof(needle)) < 0)
		return;

	char idx[4096];
	int n = sys_fread("REPO_INDEX", idx, sizeof(idx) - 1);
	if (n <= 0) {
		sys_drawtext(140, 200, "no REPO_INDEX. run 'u' first.", COLOR_DIM);
		wait_any_key(230, "press any key.", COLOR_DIM);
		return;
	}
	idx[n] = '\0';

	int y = 200;
	int line_start = 0;
	for (int i = 0; i <= n; i++) {
		if (idx[i] == '\n' || idx[i] == '\0') {
			idx[i] = '\0';
			const char *line = idx + line_start;
			if (line[0] && strstr(line, needle)) {
				sys_drawtext(140, y, line, COLOR_TEXT);
				y += 18;
				if (y > 600) break;
			}
			line_start = i + 1;
		}
	}
	wait_any_key(620, "press any key.", COLOR_DIM);
}

/* ---- install ---- */

static int unpack_package(const unsigned char *data, unsigned int len)
{
	if (len < sizeof(struct stinkpkg_hdr))
		return -1;
	const struct stinkpkg_hdr *h = (const struct stinkpkg_hdr *)data;
	if (h->magic != STINKPKG_MAGIC || h->format_ver != STINKPKG_VERSION)
		return -1;
	if (h->payload_off + h->payload_size > len)
		return -1;

	const unsigned char *cur = data + sizeof(*h) +
	                           h->dep_count * sizeof(struct stinkpkg_dep);
	const struct stinkpkg_file *files = (const struct stinkpkg_file *)cur;

	/* Write a "pkg-<name>.lst" with the file names so cmd_remove can
	 * clean up later. */
	char listname[64];
	listname[0] = 'p'; listname[1] = 'k'; listname[2] = 'g'; listname[3] = '-';
	int li = 4;
	for (int i = 0; i < STINKPKG_NAME_LEN && h->name[i]; i++)
		listname[li++] = h->name[i];
	listname[li++] = '.'; listname[li++] = 'l'; listname[li++] = 's'; listname[li++] = 't';
	listname[li] = '\0';

	char manifest[1024];
	int  m = 0;
	for (unsigned int i = 0; i < h->file_count; i++) {
		const struct stinkpkg_file *f = &files[i];
		const unsigned char *body = data + h->payload_off + f->offset;
		if (body + f->size > data + len)
			return -1;
		if (sys_fwrite(f->name, body, f->size) != 0)
			return -1;
		for (int k = 0; k < STINKPKG_FILE_LEN && f->name[k]; k++) {
			if ((unsigned)m + 1 >= sizeof(manifest)) break;
			manifest[m++] = f->name[k];
		}
		if ((unsigned)m + 1 < sizeof(manifest)) manifest[m++] = '\n';
	}
	manifest[m] = '\0';
	sys_fwrite(listname, manifest, (unsigned int)m);

	/* Append "<name> <version>\n" to STINKDB. */
	char dbline[64];
	int di = 0;
	for (int i = 0; i < STINKPKG_NAME_LEN && h->name[i]; i++)
		dbline[di++] = h->name[i];
	dbline[di++] = ' ';
	for (int i = 0; i < STINKPKG_VER_LEN && h->version[i]; i++)
		dbline[di++] = h->version[i];
	dbline[di++] = '\n';
	sys_fappend("STINKDB", dbline, (unsigned int)di);
	return 0;
}

/* Look up the SHA-256 hex digest the repo index publishes for 'name'. The
 * cached index (written by 'u') has one "<name> <version> <sha256>" line per
 * package. Copies the 64-char hash into out_hex (NUL-terminated). Returns 0
 * on success, -1 if there is no index, no matching line, or a malformed hash
 * (any of which must block the install). */
static int index_sha(const char *name, char *out_hex)
{
	static char idx[4096];
	int n = sys_fread("REPO_INDEX", idx, sizeof(idx) - 1);
	if (n <= 0)
		return -1;
	idx[n] = '\0';

	int ls = 0;
	for (int i = 0; i <= n; i++) {
		if (idx[i] != '\n' && idx[i] != '\0')
			continue;
		idx[i] = '\0';
		const char *line = idx + ls;
		ls = i + 1;

		int k = 0;
		while (name[k] && line[k] == name[k])
			k++;
		if (name[k] != '\0' || line[k] != ' ')
			continue;                       /* first token != name */

		const char *p = line + k;
		while (*p == ' ') p++;                   /* skip to version    */
		while (*p && *p != ' ') p++;             /* skip version       */
		while (*p == ' ') p++;                   /* skip to sha256     */

		int h = 0;
		while (h < 64 && p[h] && p[h] != ' ') {
			out_hex[h] = p[h];
			h++;
		}
		out_hex[h] = '\0';
		return h == 64 ? 0 : -1;
	}
	return -1;
}

/* Hex-encode a 32-byte digest into a 65-byte (64 + NUL) lowercase string. */
static void hex32(const unsigned char *d, char *out)
{
	static const char hx[] = "0123456789abcdef";
	for (int i = 0; i < 32; i++) {
		out[i * 2]     = hx[d[i] >> 4];
		out[i * 2 + 1] = hx[d[i] & 0x0f];
	}
	out[64] = '\0';
}

static void cmd_install(void)
{
	draw_header("install");
	sys_drawtext(140, 150, "package name:", COLOR_DIM);
	char name[32];
	if (read_line(280, 150, name, sizeof(name)) < 0)
		return;

	char url[160];
	int u = 0;
	const char *base = REPO_URL_BASE REPO_PKG_PATH;
	while (base[u]) { url[u] = base[u]; u++; }
	for (int i = 0; name[i] && u + 8 < (int)sizeof(url); i++)
		url[u++] = name[i];
	const char *suf = ".stinkpkg";
	for (int i = 0; suf[i] && u + 1 < (int)sizeof(url); i++)
		url[u++] = suf[i];
	url[u] = '\0';

	sys_drawtext(140, 180, "GET", COLOR_DIM);
	sys_drawtext(180, 180, url, COLOR_DIM);

	int status = 0;
	int n = http_get(url, pkg_buf, sizeof(pkg_buf), &status);
	if (n <= 0 || status != 200) {
		sys_drawtext(140, 220, "download failed.", COLOR_ERR);
		wait_any_key(250, "press any key.", COLOR_DIM);
		return;
	}

	/* Integrity gate: the package is unknown bytes off the network until it
	 * matches the SHA-256 the repo index publishes for this name. Fail closed
	 * -- a missing index, missing entry or any mismatch blocks the install so
	 * a tampered or corrupted ELF never reaches the disk or Ring 3. */
	char want[65];
	if (index_sha(name, want) != 0) {
		sys_drawtext(140, 220, "no published hash; run 'u'. refusing.", COLOR_ERR);
		wait_any_key(250, "press any key.", COLOR_DIM);
		return;
	}
	unsigned char digest[32];
	sha256(pkg_buf, (unsigned int)n, digest);
	char got[65];
	hex32(digest, got);
	if (strcasecmp(got, want) != 0) {
		sys_drawtext(140, 220, "sha256 mismatch! refusing install.", COLOR_ERR);
		wait_any_key(250, "press any key.", COLOR_DIM);
		return;
	}
	sys_drawtext(140, 220, "sha256 verified.", COLOR_OK);

	if (unpack_package(pkg_buf, (unsigned int)n) != 0) {
		sys_drawtext(140, 240, "unpack failed (bad format).", COLOR_ERR);
		wait_any_key(270, "press any key.", COLOR_DIM);
		return;
	}
	sys_drawtext(140, 240, "installed.", COLOR_OK);
	wait_any_key(270, "press any key.", COLOR_DIM);
}

/* ---- remove ---- */

static int starts_with(const char *s, const char *prefix)
{
	for (int i = 0; prefix[i]; i++)
		if (s[i] != prefix[i]) return 0;
	return 1;
}

static void cmd_remove(void)
{
	draw_header("remove");
	sys_drawtext(140, 150, "package name:", COLOR_DIM);
	char name[32];
	if (read_line(280, 150, name, sizeof(name)) < 0)
		return;

	/* Read pkg-<name>.lst to find what to delete. */
	char listname[64];
	int li = 0;
	const char *pfx = "pkg-";
	while (pfx[li]) { listname[li] = pfx[li]; li++; }
	for (int i = 0; name[i] && li + 6 < (int)sizeof(listname); i++)
		listname[li++] = name[i];
	const char *suf = ".lst";
	for (int i = 0; suf[i]; i++) listname[li++] = suf[i];
	listname[li] = '\0';

	char manifest[1024];
	int n = sys_fread(listname, manifest, sizeof(manifest) - 1);
	if (n <= 0) {
		sys_drawtext(140, 200, "package not installed.", COLOR_ERR);
		wait_any_key(230, "press any key.", COLOR_DIM);
		return;
	}
	manifest[n] = '\0';

	int removed = 0;
	int line_start = 0;
	for (int i = 0; i <= n; i++) {
		if (manifest[i] == '\n' || manifest[i] == '\0') {
			manifest[i] = '\0';
			if (i > line_start) {
				sys_fdelete(manifest + line_start);
				removed++;
			}
			line_start = i + 1;
		}
	}
	sys_fdelete(listname);

	/* Strip the pkg out of STINKDB by rewriting the rest. */
	char db[2048];
	int dn = sys_fread("STINKDB", db, sizeof(db) - 1);
	if (dn > 0) {
		db[dn] = '\0';
		char rebuilt[2048];
		int  rn = 0;
		int  ls = 0;
		for (int i = 0; i <= dn; i++) {
			if (db[i] == '\n' || db[i] == '\0') {
				db[i] = '\0';
				if (!starts_with(db + ls, name) ||
				    (db[ls + (int)strlen(name)] != ' ')) {
					for (int k = ls; k < i; k++)
						rebuilt[rn++] = db[k];
					rebuilt[rn++] = '\n';
				}
				ls = i + 1;
			}
		}
		sys_fwrite("STINKDB", rebuilt, (unsigned int)rn);
	}

	char num[16];
	uitoa((unsigned int)removed, 10, num);
	sys_drawtext(140, 200, "files removed:", COLOR_OK);
	sys_drawtext(300, 200, num, COLOR_OK);
	wait_any_key(240, "press any key.", COLOR_DIM);
}

void main(void)
{
	pkg_buf = malloc(MAX_PKG_BYTES);
	if (!pkg_buf) {
		draw_header("init failed");
		sys_drawtext(140, 150, "out of memory: needed 4 MiB for buffer.", COLOR_ERR);
		sys_sleep_ms(2000);
		sys_exit();
	}

	for (;;) {
		show_menu();
		int c = sys_getkey();
		while (c == 0) { sys_sleep_ms(20); c = sys_getkey(); }
		switch (c) {
		case 'l': cmd_list();    break;
		case 'u': cmd_update();  break;
		case 's': cmd_search();  break;
		case 'i': cmd_install(); break;
		case 'r': cmd_remove();  break;
		case 27:
		case 'q':
			sys_exit();
		default: break;
		}
	}
}
