/* Userland formatted output. One core routine -- vsnprintf -- with the rest of
 * the printf family expressed in terms of it. Supports %d %i %u %x %X %o %s
 * %c %p %% with the '-', '0' and '#' flags, an optional width, an optional
 * precision (literal or '*'), and the 'l'/'ll' length modifiers (treated as
 * int on this 32-bit target). No floats: Doom is fixed-point, and the kernel
 * has no FPU state save, so this is a deliberate omission. */
#include "libstink.h"

#define FLAG_LEFT   0x01            /* '-' : left-justify within width */
#define FLAG_ZERO   0x02            /* '0' : zero-pad numbers          */
#define FLAG_HASH   0x04            /* '#' : 0x prefix on %x, 0 on %o  */
#define FLAG_PLUS   0x08            /* '+' : show sign on positive %d  */
#define FLAG_SPACE  0x10            /* ' ' : leading space on positive */

struct buf {
	char        *out;
	unsigned int pos;
	unsigned int cap;                  /* total capacity including terminator */
};

static void buf_putc(struct buf *b, char c)
{
	if (b->pos + 1 < b->cap)
		b->out[b->pos] = c;
	b->pos++;
}

static void buf_pad(struct buf *b, char fill, int n)
{
	for (int i = 0; i < n; i++)
		buf_putc(b, fill);
}

/* Renders an unsigned magnitude into 'out' (no NUL), returns its length. The
 * caller passes the prefix character separately so flags and padding stay
 * symmetric for signed and unsigned conversions. */
static int format_u(unsigned int v, unsigned int base, int upper, char *out)
{
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	char tmp[32];
	int n = 0;

	if (v == 0) {
		out[0] = '0';
		return 1;
	}
	while (v > 0 && n < 32) {
		tmp[n++] = digits[v % base];
		v /= base;
	}
	for (int i = 0; i < n; i++)
		out[i] = tmp[n - 1 - i];
	return n;
}

static int parse_int(const char **f)
{
	int v = 0;
	while (**f >= '0' && **f <= '9') {
		v = v * 10 + (**f - '0');
		(*f)++;
	}
	return v;
}

int vsnprintf(char *out, unsigned int cap, const char *fmt, va_list args)
{
	struct buf b = { out, 0, cap };

	for (; *fmt != '\0'; fmt++) {
		if (*fmt != '%') {
			buf_putc(&b, *fmt);
			continue;
		}

		fmt++;                             /* over the '%' */

		/* ---- flags ---- */
		int flags = 0;
		for (;; fmt++) {
			if (*fmt == '-')      flags |= FLAG_LEFT;
			else if (*fmt == '0') flags |= FLAG_ZERO;
			else if (*fmt == '#') flags |= FLAG_HASH;
			else if (*fmt == '+') flags |= FLAG_PLUS;
			else if (*fmt == ' ') flags |= FLAG_SPACE;
			else                  break;
		}

		/* ---- width ---- */
		int width = 0;
		if (*fmt == '*') {
			width = va_arg(args, int);
			if (width < 0) { flags |= FLAG_LEFT; width = -width; }
			fmt++;
		} else {
			width = parse_int(&fmt);
		}

		/* ---- precision ---- */
		int prec = -1;
		if (*fmt == '.') {
			fmt++;
			if (*fmt == '*') {
				prec = va_arg(args, int);
				if (prec < 0) prec = -1;
				fmt++;
			} else {
				prec = parse_int(&fmt);
			}
		}

		/* ---- length modifiers (consumed but treated as int) ---- */
		while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z')
			fmt++;

		/* ---- conversion ---- */
		char        digits[33];
		int         dlen = 0;
		const char *prefix = "";
		int         prefix_len = 0;
		char        single;

		switch (*fmt) {
		case 'd':
		case 'i': {
			int v = va_arg(args, int);
			unsigned int mag;
			if (v < 0) {
				prefix = "-"; prefix_len = 1;
				mag = (unsigned int)(-(long)v);
			} else {
				mag = (unsigned int)v;
				if (flags & FLAG_PLUS) { prefix = "+"; prefix_len = 1; }
				else if (flags & FLAG_SPACE) { prefix = " "; prefix_len = 1; }
			}
			dlen = format_u(mag, 10, 0, digits);
			if (prec >= 0)
				flags &= ~FLAG_ZERO;       /* precision wins over '0' */
			break;
		}
		case 'u':
			dlen = format_u(va_arg(args, unsigned int), 10, 0, digits);
			if (prec >= 0) flags &= ~FLAG_ZERO;
			break;
		case 'x':
		case 'X': {
			unsigned int v = va_arg(args, unsigned int);
			dlen = format_u(v, 16, *fmt == 'X', digits);
			if (flags & FLAG_HASH && v != 0) {
				prefix = (*fmt == 'X') ? "0X" : "0x";
				prefix_len = 2;
			}
			if (prec >= 0) flags &= ~FLAG_ZERO;
			break;
		}
		case 'o': {
			unsigned int v = va_arg(args, unsigned int);
			dlen = format_u(v, 8, 0, digits);
			if (flags & FLAG_HASH && (dlen == 0 || digits[0] != '0')) {
				prefix = "0"; prefix_len = 1;
			}
			if (prec >= 0) flags &= ~FLAG_ZERO;
			break;
		}
		case 'p': {
			unsigned int v = (unsigned int)va_arg(args, void *);
			dlen = format_u(v, 16, 0, digits);
			prefix = "0x"; prefix_len = 2;
			break;
		}
		case 'c':
			single = (char)va_arg(args, int);
			/* one-character "string" so width/flags follow the same path */
			digits[0] = single; dlen = 1;
			goto fixed_string;
		case 's': {
			const char *s = va_arg(args, const char *);
			if (!s) s = "(null)";
			int slen = 0;
			while (s[slen] != '\0' && (prec < 0 || slen < prec))
				slen++;
			int pad = width - slen;
			if (!(flags & FLAG_LEFT)) buf_pad(&b, ' ', pad);
			for (int i = 0; i < slen; i++)
				buf_putc(&b, s[i]);
			if (flags & FLAG_LEFT) buf_pad(&b, ' ', pad);
			continue;
		}
		case '%':
			buf_putc(&b, '%');
			continue;
		default:
			buf_putc(&b, '%');
			buf_putc(&b, *fmt);
			continue;
		}

		/* ---- shared numeric emit path ---- */
		{
			int zero_pad = 0;
			int prec_pad = 0;
			if (prec > dlen) prec_pad = prec - dlen;
			int body = prefix_len + prec_pad + dlen;
			int width_pad = width > body ? width - body : 0;

			if ((flags & FLAG_ZERO) && !(flags & FLAG_LEFT) && prec < 0) {
				zero_pad = width_pad;
				width_pad = 0;
			}
			if (!(flags & FLAG_LEFT))
				buf_pad(&b, ' ', width_pad);
			for (int i = 0; i < prefix_len; i++)
				buf_putc(&b, prefix[i]);
			buf_pad(&b, '0', zero_pad);
			buf_pad(&b, '0', prec_pad);
			for (int i = 0; i < dlen; i++)
				buf_putc(&b, digits[i]);
			if (flags & FLAG_LEFT)
				buf_pad(&b, ' ', width_pad);
		}
		continue;

fixed_string:
		{
			int pad = width - dlen;
			if (!(flags & FLAG_LEFT)) buf_pad(&b, ' ', pad);
			for (int i = 0; i < dlen; i++)
				buf_putc(&b, digits[i]);
			if (flags & FLAG_LEFT) buf_pad(&b, ' ', pad);
		}
	}

	if (b.cap > 0)
		b.out[b.pos < b.cap ? b.pos : b.cap - 1] = '\0';
	return (int)b.pos;
}

int snprintf(char *out, unsigned int cap, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	int n = vsnprintf(out, cap, fmt, args);
	va_end(args);
	return n;
}

/* Bounded only by the destination buffer the caller chose; the standard
 * sprintf is the same call with cap = UINT_MAX. */
int vsprintf(char *out, const char *fmt, va_list args)
{
	return vsnprintf(out, 0x7FFFFFFFu, fmt, args);
}

int sprintf(char *out, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	int n = vsnprintf(out, 0x7FFFFFFFu, fmt, args);
	va_end(args);
	return n;
}

/* Renders the formatted line into a stack buffer and sends it to the kernel
 * serial debug log in one sys_log call. 512 bytes is plenty for log lines
 * and keeps stack usage tame. */
void sys_printf(const char *fmt, ...)
{
	char buf[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	sys_log(buf);
}

/* C-standard printf: same destination as sys_printf (no real stdout fd), but
 * returns a character count so ported code that checks the return value is
 * happy. */
int printf(const char *fmt, ...)
{
	char buf[512];
	va_list args;
	va_start(args, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	sys_log(buf);
	return n;
}
