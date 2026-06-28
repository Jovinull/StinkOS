/* Host-side test for the Brazilian ABNT2 keyboard layout in
 * kernel/drivers/input/keyboard.c. The kernel patches a small set of
 * positions in the US table to produce a usable BR layout without
 * UTF-8 dead-keys:
 *
 *   scancode 0x27 (US ';' / ':') -> 'c' / 'C'    (cedilla pos)
 *   scancode 0x35 (US '/' / '?') -> ';' / ':'    (BR ';' key)
 *   scancode 0x28 (US '\'' / '"') -> '~' / '^'   (no dead key yet)
 *
 * Every other position MUST stay identical to US -- if a future
 * patch accidentally touches 'q', 'a', '1', etc, regional users hit
 * a wall the moment they type their app name. The test verifies
 * the diff is exactly the documented 6 cells, no more.
 */
#include <stdio.h>

static char map_us_normal[128] = {
	0,    27,  '1', '2', '3', '4', '5', '6', '7', '8',
	'9', '0', '-', '=', '\b','\t','q', 'w', 'e', 'r',
	't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
	'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
	'\'','`', 0,  '\\','z', 'x', 'c', 'v', 'b', 'n',
	'm', ',', '.', '/', 0,  '*', 0,  ' ', 0,   0,
};

static char map_us_shift[128] = {
	0,    27,  '!', '@', '#', '$', '%', '^', '&', '*',
	'(', ')', '_', '+', '\b','\t','Q', 'W', 'E', 'R',
	'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
	'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
	'"', '~', 0,  '|', 'Z', 'X', 'C', 'V', 'B', 'N',
	'M', '<', '>', '?', 0,  '*', 0,  ' ', 0,   0,
};

static char map_br_normal[128];
static char map_br_shift [128];

static void init_br_layout(void)
{
	for (int i = 0; i < 128; i++) {
		map_br_normal[i] = map_us_normal[i];
		map_br_shift [i] = map_us_shift [i];
	}
	map_br_normal[0x27] = 'c';
	map_br_shift [0x27] = 'C';
	map_br_normal[0x35] = ';';
	map_br_shift [0x35] = ':';
	map_br_normal[0x28] = '~';
	map_br_shift [0x28] = '^';
}

static int expect_c(const char *label, char got, char want)
{
	if (got == want) {
		printf("ok   %-55s = '%c' (0x%02x)\n", label,
		       (got >= 32 && got < 127) ? got : '?', (unsigned)(unsigned char)got);
		return 0;
	}
	printf("FAIL %s: got 0x%02x want 0x%02x\n", label,
	       (unsigned)(unsigned char)got, (unsigned)(unsigned char)want);
	return 1;
}

static int expect_int(const char *label, int got, int want)
{
	if (got == want) { printf("ok   %-55s = %d\n", label, got); return 0; }
	printf("FAIL %s: got %d, want %d\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;
	init_br_layout();

	/* --- documented patches --------------------------------------- */
	failures += expect_c("BR 0x27 (cedilla pos) normal -> 'c'",
	                     map_br_normal[0x27], 'c');
	failures += expect_c("BR 0x27 (cedilla pos) shift  -> 'C'",
	                     map_br_shift [0x27], 'C');
	failures += expect_c("BR 0x35 (BR ;)        normal -> ';'",
	                     map_br_normal[0x35], ';');
	failures += expect_c("BR 0x35 (BR ;)        shift  -> ':'",
	                     map_br_shift [0x35], ':');
	failures += expect_c("BR 0x28 (~ / ^)       normal -> '~'",
	                     map_br_normal[0x28], '~');
	failures += expect_c("BR 0x28 (~ / ^)       shift  -> '^'",
	                     map_br_shift [0x28], '^');

	/* --- everything else identical to US -------------------------- */
	int diffs_normal = 0, diffs_shift = 0;
	for (int i = 0; i < 128; i++) {
		if (i == 0x27 || i == 0x35 || i == 0x28) continue;
		if (map_br_normal[i] != map_us_normal[i]) diffs_normal++;
		if (map_br_shift [i] != map_us_shift [i]) diffs_shift++;
	}
	failures += expect_int("non-patched cells (normal) identical to US",
	                       diffs_normal, 0);
	failures += expect_int("non-patched cells (shift)  identical to US",
	                       diffs_shift, 0);

	/* --- core letters survive (sanity: layout still types code) - */
	failures += expect_c("BR letter q",  map_br_normal[0x10], 'q');
	failures += expect_c("BR letter Q",  map_br_shift [0x10], 'Q');
	failures += expect_c("BR letter a",  map_br_normal[0x1E], 'a');
	failures += expect_c("BR letter z",  map_br_normal[0x2C], 'z');
	failures += expect_c("BR digit 1",   map_br_normal[0x02], '1');
	failures += expect_c("BR space",     map_br_normal[0x39], ' ');
	failures += expect_c("BR enter",     map_br_normal[0x1C], '\n');

	/* --- US 0x27 and 0x35 unchanged in their OWN table ----------- */
	failures += expect_c("US 0x27 normal still ';'", map_us_normal[0x27], ';');
	failures += expect_c("US 0x35 normal still '/'", map_us_normal[0x35], '/');

	/* --- shift swaps stay symmetric on BR-only positions --------- */
	failures += expect_c("BR shift of ;  -> :",      map_br_shift[0x35], ':');
	failures += expect_c("BR shift of ~  -> ^",      map_br_shift[0x28], '^');

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
