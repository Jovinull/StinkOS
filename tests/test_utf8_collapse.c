/* Host-side test for the UTF-8 collapse done by log_push in
 * apps/shell.c. The shell scrollback can only render single-byte
 * page-437 glyphs, so any UTF-8 multi-byte sequence is collapsed to
 * one '?' placeholder. Continuation bytes following a lead are
 * swallowed; stray continuations are dropped silently.
 *
 * Cases the logic must handle without rendering garbage:
 *   - Pure ASCII: copied verbatim
 *   - 2-byte UTF-8 (e.g. á = 0xC3 0xA1): one '?'
 *   - 3-byte UTF-8 (e.g. € = 0xE2 0x82 0xAC): one '?'
 *   - 4-byte UTF-8 (e.g. emoji = 0xF0 0x9F 0x98 0x80): one '?'
 *   - Mixed ASCII + multi-byte
 *   - Stray continuation (0x80..0xBF without lead): dropped
 *   - Truncated multi-byte at end of buffer: still one '?'
 */
#include <stdio.h>
#include <string.h>

#define TERM_COLS 128

/* Mirror of log_push's character-translation loop -- writes to `out`
 * the collapsed scrollback line, NUL-terminated. */
static void collapse(const char *s, char *out)
{
	unsigned int oi = 0;
	unsigned int i = 0;
	while (s[i] && oi < TERM_COLS) {
		unsigned char c = (unsigned char)s[i];
		if (c < 0x80u) {
			out[oi++] = (char)c;
			i++;
		} else if ((c & 0xC0u) == 0xC0u) {
			out[oi++] = '?';
			i++;
			while ((unsigned char)s[i] >= 0x80u &&
			       (unsigned char)s[i] <  0xC0u)
				i++;
		} else {
			i++;       /* stray continuation */
		}
	}
	out[oi] = '\0';
}

static int expect_str(const char *label, const char *got, const char *want)
{
	if (strcmp(got, want) == 0) {
		printf("ok   %-50s = \"%s\"\n", label, got);
		return 0;
	}
	printf("FAIL %s: got \"%s\", want \"%s\"\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;
	char buf[256];

	/* Pure ASCII: copied verbatim. */
	collapse("hello world", buf);
	failures += expect_str("ASCII verbatim", buf, "hello world");

	/* Empty string. */
	collapse("", buf);
	failures += expect_str("empty input -> empty", buf, "");

	/* 2-byte UTF-8: á (0xC3 0xA1). */
	collapse("\xC3\xA1", buf);
	failures += expect_str("á -> ?", buf, "?");

	/* 2-byte UTF-8 surrounded by ASCII. */
	collapse("ol\xC3\xA1!", buf);
	failures += expect_str("olá! -> ol?!", buf, "ol?!");

	/* 3-byte UTF-8: € (0xE2 0x82 0xAC). */
	collapse("\xE2\x82\xAC", buf);
	failures += expect_str("€ -> ?", buf, "?");

	/* 4-byte UTF-8: emoji (😀 = 0xF0 0x9F 0x98 0x80). */
	collapse("\xF0\x9F\x98\x80", buf);
	failures += expect_str("😀 -> ?", buf, "?");

	/* Multiple multi-byte sequences. */
	collapse("\xC3\xA1\xC3\xA9\xC3\xAD", buf);    /* áéí */
	failures += expect_str("áéí -> ???", buf, "???");

	/* Mixed run: hello + 1 emoji + world. */
	collapse("hi \xF0\x9F\x98\x80 there", buf);
	failures += expect_str("'hi 😀 there' -> 'hi ? there'", buf, "hi ? there");

	/* Stray continuation byte alone: dropped. */
	collapse("\x80", buf);
	failures += expect_str("stray 0x80 -> empty", buf, "");

	collapse("a\x80\x80\x80""b", buf);
	failures += expect_str("strays between ASCII dropped", buf, "ab");

	/* Truncated multi-byte at end (no continuation): still one '?'. */
	collapse("\xC3", buf);
	failures += expect_str("truncated lead -> ?", buf, "?");

	collapse("hi \xE2", buf);
	failures += expect_str("'hi ' + truncated lead", buf, "hi ?");

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
