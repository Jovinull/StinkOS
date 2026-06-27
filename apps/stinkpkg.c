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

#define DEFAULT_REPO_URL "http://stinkos-repo.local"
#define REPO_INDEX_PATH  "/index.txt"
#define REPO_PKG_PATH    "/pkg/"
#define REPO_URL_MAX     128

#define MAX_PKG_BYTES   (4u * 1024u * 1024u)   /* 4 MiB cap per package */

/* Configurable repository base URL.
 *
 * Resolution order (first hit wins):
 *   1. The "repo_url=..." line of STINKPKG.CONF in StinkFS.
 *   2. The compile-time DEFAULT_REPO_URL.
 *
 * The config file is plain text, key=value per line; unknown keys are
 * ignored. Caching keeps the cost to one sys_fread for the lifetime of
 * the process. */
static char  repo_url_cache[REPO_URL_MAX];
static int   repo_url_loaded;

static const char *repo_base(void)
{
	if (repo_url_loaded)
		return repo_url_cache;

	char conf[512];
	int  n = sys_fread("STINKPKG.CONF", conf, sizeof(conf) - 1);
	if (n > 0) {
		conf[n] = '\0';
		const char *key = "repo_url=";
		int klen = 9;
		const char *p = conf;
		while (*p) {
			int match = 1;
			for (int j = 0; j < klen; j++) {
				if (p[j] != key[j]) {
					match = 0;
					break;
				}
			}
			if (match) {
				p += klen;
				int k = 0;
				while (*p && *p != '\n' && *p != '\r' &&
				       k < REPO_URL_MAX - 1)
					repo_url_cache[k++] = *p++;
				repo_url_cache[k] = '\0';
				if (k > 0) {
					repo_url_loaded = 1;
					return repo_url_cache;
				}
				break;
			}
			while (*p && *p != '\n') p++;
			if (*p == '\n') p++;
		}
	}

	const char *def = DEFAULT_REPO_URL;
	int k = 0;
	while (def[k] && k < REPO_URL_MAX - 1) {
		repo_url_cache[k] = def[k];
		k++;
	}
	repo_url_cache[k] = '\0';
	repo_url_loaded = 1;
	return repo_url_cache;
}

/* Concatenate the configured base + `suffix` into `out`. Truncates silently
 * if the destination is too small; caller picks the cap. Returns out for
 * call-chain convenience. */
static char *build_url(char *out, unsigned int cap, const char *suffix)
{
	const char *base = repo_base();
	unsigned int o = 0;
	while (base[o] && o + 1 < cap) {
		out[o] = base[o];
		o++;
	}
	for (unsigned int i = 0; suffix[i] && o + 1 < cap; i++)
		out[o++] = suffix[i];
	out[o] = '\0';
	return out;
}

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
	sys_drawtext(160, 215, "I  install without resolving deps",  COLOR_DIM);
	sys_drawtext(160, 222, "R  replay STINKPKG.LCK lockfile",    COLOR_DIM);
	sys_drawtext(160, 229, "V  verify STINKDB vs lockfile",      COLOR_DIM);
	sys_drawtext(140, 230, "g  upgrade installed packages",    COLOR_TEXT);
	sys_drawtext(140, 250, "r  remove an installed package",   COLOR_TEXT);
	sys_drawtext(140, 270, "y  inspect a .stinkpkg in StinkFS",COLOR_TEXT);
	sys_drawtext(140, 310, "esc / q  exit",                    COLOR_DIM);
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
	char idx_url[REPO_URL_MAX + 32];
	build_url(idx_url, sizeof(idx_url), REPO_INDEX_PATH);

	sys_drawtext(140, 150, "GET", COLOR_DIM);
	sys_drawtext(180, 150, idx_url, COLOR_DIM);

	int status = 0;
	int n = http_get(idx_url, pkg_buf, MAX_PKG_BYTES, &status);
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

/* True if `target` matches the start of any line of `text` followed by NUL
 * or newline. Used to test if a candidate filename appears in a manifest. */
static int line_starts_with(const char *text, int text_len, const char *target)
{
	int ls = 0;
	for (int i = 0; i <= text_len; i++) {
		if (i == text_len || text[i] == '\n' || text[i] == '\0') {
			int k = 0;
			while (target[k] && (ls + k) < i && text[ls + k] == target[k])
				k++;
			if (target[k] == '\0' && (ls + k == i))
				return 1;
			ls = i + 1;
		}
	}
	return 0;
}

/* Return 1 if any pkg-*.lst manifest other than `self_pkg`'s already claims
 * `fname`. Walks every StinkFS file via sys_finfo, fread()s any pkg-*.lst
 * (cap 1024 bytes per manifest) and scans line-by-line. */
static int filename_claimed_elsewhere(const char *fname, const char *self_pkg)
{
	int count = sys_fcount();
	for (int i = 0; i < count; i++) {
		char en[16];
		if (sys_finfo(i, en) < 0)
			continue;
		/* match pkg-<*>.lst pattern; ignore other StinkFS entries */
		if (en[0] != 'p' || en[1] != 'k' || en[2] != 'g' || en[3] != '-')
			continue;
		int el = 0;
		while (en[el] && el < 16) el++;
		if (el < 8) continue;
		if (en[el - 4] != '.' || en[el - 3] != 'l' ||
		    en[el - 2] != 's' || en[el - 1] != 't')
			continue;

		/* Skip self: en = "pkg-<self_pkg>.lst" */
		int s = 0;
		while (self_pkg[s] && (4 + s) < el && en[4 + s] == self_pkg[s])
			s++;
		if (self_pkg[s] == '\0' && (4 + s) == (el - 4))
			continue;

		char manifest[1024];
		int n = sys_fread(en, manifest, sizeof(manifest));
		if (n <= 0)
			continue;
		if (line_starts_with(manifest, n, fname))
			return 1;
	}
	return 0;
}

static int unpack_package(const unsigned char *data, unsigned int len)
{
	if (len < sizeof(struct stinkpkg_hdr))
		return -1;
	const struct stinkpkg_hdr *h = (const struct stinkpkg_hdr *)data;
	if (h->magic != STINKPKG_MAGIC || h->format_ver != STINKPKG_VERSION)
		return -1;
	if (h->payload_off + h->payload_size > len)
		return -1;
	if (h->flags & STINKPKG_FLAG_COMPRESSED) {
		/* Compressed payloads require an inflate decoder that has not
		 * landed yet -- refuse with a clear marker so the caller knows
		 * to either rebuild the package without --compress or wait for
		 * the decoder commit. */
		return -1;
	}

	const unsigned char *cur = data + sizeof(*h) +
	                           h->dep_count * sizeof(struct stinkpkg_dep);
	const struct stinkpkg_file *files = (const struct stinkpkg_file *)cur;

	/* Conflict detection: refuse to clobber a file claimed by another
	 * package. Two packages co-installing the same filename almost always
	 * means one overwrites the other on next upgrade, breaking remove(). */
	char this_pkg[STINKPKG_NAME_LEN];
	for (int k = 0; k < STINKPKG_NAME_LEN; k++)
		this_pkg[k] = h->name[k];
	this_pkg[STINKPKG_NAME_LEN - 1] = '\0';
	for (unsigned int i = 0; i < h->file_count; i++) {
		char fname[STINKPKG_FILE_LEN + 1];
		for (int k = 0; k < STINKPKG_FILE_LEN; k++)
			fname[k] = files[i].name[k];
		fname[STINKPKG_FILE_LEN] = '\0';
		if (filename_claimed_elsewhere(fname, this_pkg))
			return -1;
	}

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

/* ---- install (with recursive dep resolution) ---- */

#define INSTALL_MAX_DEPTH   4               /* guards against pkg cycles */

/* Lookup in STINKDB: returns 1 if "<name> " appears at the start of any line. */
static int pkg_installed(const char *name)
{
	char db[2048];
	int dn = sys_fread("STINKDB", db, sizeof(db) - 1);
	if (dn <= 0)
		return 0;
	db[dn] = '\0';
	int ls = 0;
	for (int i = 0; i <= dn; i++) {
		if (db[i] != '\n' && db[i] != '\0')
			continue;
		db[i] = '\0';
		const char *line = db + ls;
		int k = 0;
		while (name[k] && line[k] == name[k]) k++;
		if (name[k] == '\0' && line[k] == ' ')
			return 1;
		ls = i + 1;
	}
	return 0;
}

/* Download <name>.stinkpkg into pkg_buf and verify its SHA-256 against the
 * repo index. Returns the byte count on success, -1 on any failure with a
 * one-line error already drawn at y. */
static int fetch_and_verify(const char *name, int y)
{
	char url[REPO_URL_MAX + 64];
	build_url(url, sizeof(url), REPO_PKG_PATH);
	int u = 0;
	while (url[u]) u++;
	for (int i = 0; name[i] && u + 12 < (int)sizeof(url); i++)
		url[u++] = name[i];
	const char *suf = ".stinkpkg";
	for (int i = 0; suf[i] && u + 1 < (int)sizeof(url); i++)
		url[u++] = suf[i];
	url[u] = '\0';

	int status = 0;
	int n = http_get(url, pkg_buf, MAX_PKG_BYTES, &status);
	if (n <= 0 || status != 200) {
		sys_drawtext(140, y, "download failed.", COLOR_ERR);
		return -1;
	}

	char want[65];
	if (index_sha(name, want) != 0) {
		sys_drawtext(140, y, "no published hash; refusing.", COLOR_ERR);
		return -1;
	}
	unsigned char digest[32];
	sha256(pkg_buf, (unsigned int)n, digest);
	char got[65];
	hex32(digest, got);
	if (strcasecmp(got, want) != 0) {
		sys_drawtext(140, y, "sha256 mismatch! refusing.", COLOR_ERR);
		return -1;
	}
	return n;
}

/* Read the pinned version for `name` out of STINKPKG.PIN (one "name version"
 * line per pin). Writes into out_ver, returns 0 on hit, -1 if no pin file
 * or no matching line. STINKPKG.PIN is user-editable -- there is no auto-
 * generated pin today; the user pins by writing the file with the shell. */
static int pin_get(const char *name, char *out_ver, unsigned int cap)
{
	char pinned[1024];
	int  n = sys_fread("STINKPKG.PIN", pinned, sizeof(pinned) - 1);
	if (n <= 0)
		return -1;
	pinned[n] = '\0';

	int ls = 0;
	for (int i = 0; i <= n; i++) {
		if (pinned[i] != '\n' && pinned[i] != '\0')
			continue;
		pinned[i] = '\0';
		const char *line = pinned + ls;
		int k = 0;
		while (name[k] && line[k] == name[k]) k++;
		if (name[k] == '\0' && line[k] == ' ') {
			const char *p = line + k + 1;
			unsigned int w = 0;
			while (*p && *p != ' ' && w + 1 < cap)
				out_ver[w++] = *p++;
			out_ver[w] = '\0';
			return 0;
		}
		ls = i + 1;
	}
	return -1;
}

/* Append a lockfile entry for the install we just verified. Format mirrors
 * the repo index -- "<name> <version> <sha256>\n" -- so the lockfile can
 * be used as a self-contained reproducibility record (replay the same set
 * of installs against a different mirror by feeding STINKPKG.LCK back to
 * stink-pkg via a future "stink-pkg replay" subcommand). */
static void lockfile_append(const char *name, const char *version,
                            const char *sha_hex)
{
	char line[STINKPKG_NAME_LEN + STINKPKG_VER_LEN + 64 + 8];
	unsigned int p = 0;
	for (int i = 0; name[i] && p < sizeof(line) - 1; i++)
		line[p++] = name[i];
	if (p < sizeof(line) - 1) line[p++] = ' ';
	for (int i = 0; version[i] && p < sizeof(line) - 1; i++)
		line[p++] = version[i];
	if (p < sizeof(line) - 1) line[p++] = ' ';
	for (int i = 0; sha_hex[i] && p < sizeof(line) - 1; i++)
		line[p++] = sha_hex[i];
	if (p < sizeof(line) - 1) line[p++] = '\n';
	sys_fappend("STINKPKG.LCK", line, p);
}

/* Recursive install: walks the header's dependency list, installs anything
 * missing from STINKDB depth-first, then unpacks the requested package.
 * Re-fetches the requested package after the dep loop because the recursive
 * dep installs share pkg_buf and clobber its contents. Depth cap blocks
 * cyclic dependency graphs. `force` skips the already-installed check so
 * the upgrade path can re-pull a package even if it already appears in
 * STINKDB. */
/* `skip_deps` short-circuits the recursive dependency loop -- used by the
 * 'I' menu key for force-installing a single package without pulling its
 * dep graph. Useful for testing and for hand-resolving conflicts. */
static int install_pkg(const char *name, int depth, int progress_y, int force,
                       int skip_deps)
{
	if (depth >= INSTALL_MAX_DEPTH) {
		sys_drawtext(140, progress_y, "dep depth limit reached.", COLOR_ERR);
		return -1;
	}
	if (!force && pkg_installed(name)) {
		sys_drawtext(140, progress_y, "already installed.", COLOR_DIM);
		sys_drawtext(280, progress_y, name, COLOR_DIM);
		return 0;
	}

	int n = fetch_and_verify(name, progress_y);
	if (n < 0)
		return -1;

	/* Honor STINKPKG.PIN: if the user pinned this package to a specific
	 * version and the repo index publishes a different one, refuse the
	 * install instead of silently upgrading past the pin. */
	{
		char pinned_ver[STINKPKG_VER_LEN];
		if (pin_get(name, pinned_ver, sizeof(pinned_ver)) == 0) {
			char idx_ver[STINKPKG_VER_LEN];
			if (index_version(name, idx_ver, sizeof(idx_ver)) == 0) {
				int same = 1;
				for (unsigned int k = 0; k < sizeof(idx_ver); k++) {
					if (pinned_ver[k] != idx_ver[k]) { same = 0; break; }
					if (pinned_ver[k] == '\0') break;
				}
				if (!same) {
					sys_drawtext(140, progress_y,
					    "pinned to a different version; refusing.",
					    COLOR_ERR);
					return -1;
				}
			}
		}
	}

	/* Snapshot dep names before any recursion clobbers pkg_buf. */
	const struct stinkpkg_hdr *h = (const struct stinkpkg_hdr *)pkg_buf;
	unsigned int dc = h->dep_count;
	if (dc > 8) dc = 8;                         /* cap; ignores excess deps */
	char deps[8][STINKPKG_NAME_LEN];
	const struct stinkpkg_dep *src = (const struct stinkpkg_dep *)
	                                 (pkg_buf + sizeof(*h));
	for (unsigned int i = 0; i < dc; i++) {
		for (int k = 0; k < STINKPKG_NAME_LEN; k++)
			deps[i][k] = src[i].name[k];
		deps[i][STINKPKG_NAME_LEN - 1] = '\0';
	}

	if (!skip_deps) {
		for (unsigned int i = 0; i < dc; i++) {
			if (install_pkg(deps[i], depth + 1, progress_y + 20, 0, 0) != 0)
				return -1;
		}
	}

	/* Re-fetch after recursion so pkg_buf again holds THIS package. */
	if (!skip_deps && dc > 0) {
		n = fetch_and_verify(name, progress_y);
		if (n < 0)
			return -1;
	}

	if (unpack_package(pkg_buf, (unsigned int)n) != 0) {
		sys_drawtext(140, progress_y, "unpack failed (bad format).", COLOR_ERR);
		return -1;
	}

	/* Record the install in STINKPKG.LCK so reproducing this exact set on
	 * another machine is one file copy away. Re-computes the SHA over the
	 * verified bytes already sitting in pkg_buf -- cheap, and the digest
	 * needs to land in the lockfile alongside the version. */
	{
		const struct stinkpkg_hdr *hh =
		    (const struct stinkpkg_hdr *)pkg_buf;
		unsigned char digest[32];
		sha256(pkg_buf, (unsigned int)n, digest);
		char sha_hex[65];
		hex32(digest, sha_hex);
		char name_z[STINKPKG_NAME_LEN + 1];
		char ver_z[STINKPKG_VER_LEN + 1];
		for (int i = 0; i < STINKPKG_NAME_LEN; i++) name_z[i] = hh->name[i];
		name_z[STINKPKG_NAME_LEN] = '\0';
		for (int i = 0; i < STINKPKG_VER_LEN; i++) ver_z[i] = hh->version[i];
		ver_z[STINKPKG_VER_LEN] = '\0';
		lockfile_append(name_z, ver_z, sha_hex);
	}
	return 0;
}

static void cmd_install(void)
{
	draw_header("install");
	sys_drawtext(140, 150, "package name:", COLOR_DIM);
	char name[32];
	if (read_line(280, 150, name, sizeof(name)) < 0)
		return;

	if (install_pkg(name, 0, 180, 0, 0) == 0)
		sys_drawtext(140, 280, "installed.", COLOR_OK);
	wait_any_key(310, "press any key.", COLOR_DIM);
}

/* Look up `name` in STINKPKG.LCK and copy its SHA into out_hex.
 * Returns 0 on hit, -1 if the lockfile is missing or the name isn't there. */
static int lock_sha(const char *name, char *out_hex)
{
	char lock[2048];
	int  n = sys_fread("STINKPKG.LCK", lock, sizeof(lock) - 1);
	if (n <= 0)
		return -1;
	lock[n] = '\0';

	int ls = 0;
	for (int i = 0; i <= n; i++) {
		if (lock[i] != '\n' && lock[i] != '\0')
			continue;
		lock[i] = '\0';
		const char *line = lock + ls;
		int k = 0;
		while (name[k] && line[k] == name[k]) k++;
		if (name[k] == '\0' && line[k] == ' ') {
			const char *p = line + k + 1;
			while (*p == ' ') p++;
			while (*p && *p != ' ') p++;           /* skip version */
			while (*p == ' ') p++;
			int h = 0;
			while (h < 64 && p[h] && p[h] != ' ') {
				out_hex[h] = p[h];
				h++;
			}
			out_hex[h] = '\0';
			return h == 64 ? 0 : -1;
		}
		ls = i + 1;
	}
	return -1;
}

/* Walk STINKDB and report each installed package's lockfile status: green
 * if STINKPKG.LCK has a matching SHA entry, dim red if not. Catches
 * packages that landed via a non-standard path (manual sys_fwrite, partial
 * install, etc) so the user knows the lockfile is incomplete. */
static void cmd_verify(void)
{
	draw_header("verify (STINKDB vs STINKPKG.LCK)");

	char db[2048];
	int  dn = sys_fread("STINKDB", db, sizeof(db) - 1);
	if (dn <= 0) {
		sys_drawtext(140, 150, "no STINKDB -- nothing installed.", COLOR_DIM);
		wait_any_key(180, "press any key.", COLOR_DIM);
		return;
	}
	db[dn] = '\0';

	int y = 150;
	int locked = 0, unlocked = 0;
	int ls = 0;
	for (int i = 0; i <= dn; i++) {
		if (db[i] != '\n' && db[i] != '\0')
			continue;
		db[i] = '\0';
		if (i > ls) {
			char name[32];
			int j = ls, k = 0;
			while (j < i && db[j] != ' ' && k + 1 < (int)sizeof(name))
				name[k++] = db[j++];
			name[k] = '\0';
			if (name[0]) {
				char sha[65];
				int rc = lock_sha(name, sha);
				if (rc == 0) {
					locked++;
					sys_drawtext(140, y, name, COLOR_OK);
					sys_drawtext(280, y, "locked  ", COLOR_OK);
					sha[16] = '\0';            /* trim for screen width */
					sys_drawtext(380, y, sha, COLOR_DIM);
				} else {
					unlocked++;
					sys_drawtext(140, y, name, COLOR_ERR);
					sys_drawtext(280, y, "no lockfile entry", COLOR_ERR);
				}
				y += 16;
				if (y > 600) break;
			}
		}
		ls = i + 1;
	}

	char num[16];
	uitoa((unsigned int)locked, 10, num);
	sys_drawtext(140, y + 8, "locked:",   COLOR_OK);
	sys_drawtext(220, y + 8, num,         COLOR_TEXT);
	uitoa((unsigned int)unlocked, 10, num);
	sys_drawtext(280, y + 8, "unlocked:", COLOR_ERR);
	sys_drawtext(380, y + 8, num,         COLOR_TEXT);
	wait_any_key(y + 40, "press any key.", COLOR_DIM);
}

/* Re-install every entry of STINKPKG.LCK that isn't already in STINKDB.
 * Each lockfile line is "<name> <version> <sha256>"; we ignore the SHA
 * (the install path re-verifies against the current repo index) and the
 * version (we install whatever the index publishes today). */
static void cmd_replay(void)
{
	draw_header("replay (re-install from lockfile)");

	char lock[2048];
	int  n = sys_fread("STINKPKG.LCK", lock, sizeof(lock) - 1);
	if (n <= 0) {
		sys_drawtext(140, 150, "no STINKPKG.LCK found.", COLOR_ERR);
		wait_any_key(180, "press any key.", COLOR_DIM);
		return;
	}
	lock[n] = '\0';

	int y = 150;
	int installed = 0;
	int skipped   = 0;
	int ls        = 0;
	for (int i = 0; i <= n; i++) {
		if (lock[i] != '\n' && lock[i] != '\0')
			continue;
		lock[i] = '\0';
		if (i > ls) {
			char name[32];
			int j = ls;
			int k = 0;
			while (j < i && lock[j] != ' ' && k + 1 < (int)sizeof(name))
				name[k++] = lock[j++];
			name[k] = '\0';
			if (name[0]) {
				if (pkg_installed(name)) {
					skipped++;
				} else {
					sys_drawtext(140, y, name, COLOR_TEXT);
					if (install_pkg(name, 0, y + 20, 0, 0) == 0)
						installed++;
					y += 60;
					if (y > 600) break;
				}
			}
		}
		ls = i + 1;
	}

	char num[16];
	uitoa((unsigned int)installed, 10, num);
	sys_drawtext(140, y, "installed:", COLOR_OK);
	sys_drawtext(280, y, num, COLOR_TEXT);
	uitoa((unsigned int)skipped, 10, num);
	sys_drawtext(140, y + 18, "already present:", COLOR_DIM);
	sys_drawtext(320, y + 18, num, COLOR_DIM);
	wait_any_key(y + 50, "press any key.", COLOR_DIM);
}

static void cmd_install_no_deps(void)
{
	draw_header("install (no deps)");
	sys_drawtext(140, 150, "package name:", COLOR_DIM);
	char name[32];
	if (read_line(280, 150, name, sizeof(name)) < 0)
		return;
	if (install_pkg(name, 0, 180, 0, 1) == 0)
		sys_drawtext(140, 280, "installed without deps.", COLOR_OK);
	wait_any_key(310, "press any key.", COLOR_DIM);
}

/* ---- upgrade ---- */

/* Strip every line of STINKDB whose first token equals `name`. Writes the
 * filtered text back (truncating). No-op if STINKDB is missing. */
static void stinkdb_remove(const char *name)
{
	char db[2048];
	int  dn = sys_fread("STINKDB", db, sizeof(db) - 1);
	if (dn <= 0)
		return;
	db[dn] = '\0';

	char out[2048];
	int  on = 0, ls = 0;
	for (int i = 0; i <= dn; i++) {
		if (db[i] != '\n' && db[i] != '\0')
			continue;
		db[i] = '\0';
		int k = 0;
		while (name[k] && db[ls + k] == name[k]) k++;
		int match = (name[k] == '\0' && db[ls + k] == ' ');
		if (!match) {
			for (int j = ls; j < i && on < (int)sizeof(out) - 1; j++)
				out[on++] = db[j];
			if (on < (int)sizeof(out)) out[on++] = '\n';
		}
		ls = i + 1;
	}
	sys_fwrite("STINKDB", out, (unsigned int)on);
}

/* Look up `name` in REPO_INDEX and copy its version into out_ver (cap bytes).
 * Returns 0 on success, -1 if no index or no entry. */
static int index_version(const char *name, char *out_ver, unsigned int cap)
{
	char idx[4096];
	int  n = sys_fread("REPO_INDEX", idx, sizeof(idx) - 1);
	if (n <= 0)
		return -1;
	idx[n] = '\0';
	int ls = 0;
	for (int i = 0; i <= n; i++) {
		if (idx[i] != '\n' && idx[i] != '\0')
			continue;
		idx[i] = '\0';
		const char *line = idx + ls;
		int k = 0;
		while (name[k] && line[k] == name[k]) k++;
		if (name[k] == '\0' && line[k] == ' ') {
			const char *p = line + k + 1;       /* version starts here */
			unsigned int w = 0;
			while (*p && *p != ' ' && w + 1 < cap)
				out_ver[w++] = *p++;
			out_ver[w] = '\0';
			return 0;
		}
		ls = i + 1;
	}
	return -1;
}

static void cmd_upgrade(void)
{
	draw_header("upgrade");

	char db[2048];
	int dn = sys_fread("STINKDB", db, sizeof(db) - 1);
	if (dn <= 0) {
		sys_drawtext(140, 150, "nothing installed.", COLOR_DIM);
		wait_any_key(180, "press any key.", COLOR_DIM);
		return;
	}
	db[dn] = '\0';

	int y = 150;
	int upgraded = 0;
	int ls = 0;
	for (int i = 0; i <= dn; i++) {
		if (db[i] != '\n' && db[i] != '\0')
			continue;
		db[i] = '\0';
		if (i > ls) {
			/* parse "<name> <version>" */
			char name[32];
			char cur_ver[16];
			int  ni = 0, j = ls;
			while (j < i && db[j] != ' ' && ni + 1 < (int)sizeof(name))
				name[ni++] = db[j++];
			name[ni] = '\0';
			while (j < i && db[j] == ' ') j++;
			int vi = 0;
			while (j < i && vi + 1 < (int)sizeof(cur_ver))
				cur_ver[vi++] = db[j++];
			cur_ver[vi] = '\0';

			char idx_ver[16];
			if (name[0] && index_version(name, idx_ver, sizeof(idx_ver)) == 0) {
				int same = 1;
				for (int k = 0; k < (int)sizeof(cur_ver); k++) {
					if (cur_ver[k] != idx_ver[k]) { same = 0; break; }
					if (cur_ver[k] == '\0') break;
				}
				if (!same) {
					sys_drawtext(140, y, name, COLOR_TEXT);
					sys_drawtext(280, y, cur_ver, COLOR_DIM);
					sys_drawtext(360, y, "->", COLOR_DIM);
					sys_drawtext(400, y, idx_ver, COLOR_OK);
					stinkdb_remove(name);
					if (install_pkg(name, 0, y + 20, 1, 0) == 0)
						upgraded++;
					y += 60;
					if (y > 600) break;
				}
			}
		}
		ls = i + 1;
	}

	if (upgraded == 0)
		sys_drawtext(140, y, "everything up to date.", COLOR_OK);
	else {
		sys_drawtext(140, y, "upgraded packages:", COLOR_OK);
		char num[16]; uitoa((unsigned int)upgraded, 10, num);
		sys_drawtext(340, y, num, COLOR_TEXT);
	}
	wait_any_key(y + 30, "press any key.", COLOR_DIM);
}

/* ---- query (inspect a downloaded .stinkpkg without installing) ---- */

static void cmd_query(void)
{
	draw_header("inspect");
	sys_drawtext(140, 150, "StinkFS filename:", COLOR_DIM);
	char fname[32];
	if (read_line(280, 150, fname, sizeof(fname)) < 0)
		return;

	int n = sys_fread(fname, pkg_buf, MAX_PKG_BYTES);
	if (n <= 0) {
		sys_drawtext(140, 190, "file not found in StinkFS.", COLOR_ERR);
		wait_any_key(220, "press any key.", COLOR_DIM);
		return;
	}

	if ((unsigned)n < sizeof(struct stinkpkg_hdr)) {
		sys_drawtext(140, 190, "too small to be a stinkpkg.", COLOR_ERR);
		wait_any_key(220, "press any key.", COLOR_DIM);
		return;
	}
	const struct stinkpkg_hdr *h = (const struct stinkpkg_hdr *)pkg_buf;
	if (h->magic != STINKPKG_MAGIC) {
		sys_drawtext(140, 190, "bad magic (not a stinkpkg).", COLOR_ERR);
		wait_any_key(220, "press any key.", COLOR_DIM);
		return;
	}
	if (h->format_ver != STINKPKG_VERSION) {
		sys_drawtext(140, 190, "unsupported format version.", COLOR_ERR);
		wait_any_key(220, "press any key.", COLOR_DIM);
		return;
	}

	int y = 190;
	char buf[16];

	sys_drawtext(140, y, "name", COLOR_DIM);
	sys_drawtext(220, y, (const char *)h->name, COLOR_TEXT);
	y += 18;

	sys_drawtext(140, y, "version", COLOR_DIM);
	sys_drawtext(220, y, (const char *)h->version, COLOR_TEXT);
	y += 18;

	sys_drawtext(140, y, "desc", COLOR_DIM);
	sys_drawtext(220, y, (const char *)h->description, COLOR_TEXT);
	y += 18;

	uitoa(h->dep_count, 10, buf);
	sys_drawtext(140, y, "deps", COLOR_DIM);
	sys_drawtext(220, y, buf, COLOR_TEXT);
	y += 18;

	uitoa(h->file_count, 10, buf);
	sys_drawtext(140, y, "files", COLOR_DIM);
	sys_drawtext(220, y, buf, COLOR_TEXT);
	y += 18;

	uitoa(h->payload_size, 10, buf);
	sys_drawtext(140, y, "payload bytes", COLOR_DIM);
	sys_drawtext(280, y, buf, COLOR_TEXT);
	y += 24;

	/* List deps (one per line, capped to screen). */
	const struct stinkpkg_dep *deps = (const struct stinkpkg_dep *)
	                                  (pkg_buf + sizeof(*h));
	for (unsigned int i = 0; i < h->dep_count && y < 600; i++) {
		sys_drawtext(160, y, "dep", COLOR_DIM);
		sys_drawtext(220, y, (const char *)deps[i].name, COLOR_TEXT);
		y += 16;
	}

	/* List files with sizes. */
	const struct stinkpkg_file *files = (const struct stinkpkg_file *)
	                                    ((const unsigned char *)deps +
	                                     h->dep_count * sizeof(struct stinkpkg_dep));
	for (unsigned int i = 0; i < h->file_count && y < 600; i++) {
		sys_drawtext(160, y, (const char *)files[i].name, COLOR_TEXT);
		uitoa(files[i].size, 10, buf);
		sys_drawtext(380, y, buf, COLOR_DIM);
		sys_drawtext(440, y, "bytes", COLOR_DIM);
		y += 16;
	}

	wait_any_key(620, "press any key.", COLOR_DIM);
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
		case 'i': cmd_install();        break;
		case 'I': cmd_install_no_deps(); break;
		case 'R': cmd_replay();         break;
		case 'V': cmd_verify();         break;
		case 'g': cmd_upgrade();        break;
		case 'y': cmd_query();          break;
		case 'r': cmd_remove();  break;
		case 27:
		case 'q':
			sys_exit();
		default: break;
		}
	}
}
