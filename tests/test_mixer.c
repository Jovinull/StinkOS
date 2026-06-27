/* Stress test for the kernel-side software mixer math (kernel/drivers/audio/
 * audio.c). Replicates the per-sample arithmetic so it can run on the host
 * without dragging in the SB16 hardware path: signed center, volume scale
 * by 256, sum, saturate, re-center. Real DSP I/O is exercised by the QEMU
 * boot test; this file just guards the math against regressions.
 *
 * Test cases:
 *   1. Single full-volume channel reproduces the input byte-for-byte.
 *   2. Zero volume yields the silence rail (0x80).
 *   3. Eight max-volume channels with peaks all aligned hit saturation
 *      (output stays in [0, 255]).
 *   4. Two opposing-phase channels cancel to silence.
 *   5. A channel that runs past its length deactivates itself.
 */
#include <stdio.h>
#include <string.h>

#define MIX_CHANNELS 8

struct mix_channel {
	const unsigned char *src;
	unsigned int         pos;
	unsigned int         length;
	int                  volume;
	int                  active;
};

static struct mix_channel channels[MIX_CHANNELS];

/* Byte-identical math copy of mixer_fill_window in audio.c. Kept in sync
 * by hand -- any change to the algorithm there should mirror here, with a
 * matching test case for the new behaviour. */
static void mixer_fill(unsigned char *out, unsigned int n)
{
	for (unsigned int i = 0; i < n; i++) {
		int sum = 0;
		for (int c = 0; c < MIX_CHANNELS; c++) {
			struct mix_channel *ch = &channels[c];
			if (!ch->active) continue;
			if (ch->pos >= ch->length) { ch->active = 0; continue; }
			int s = (int)ch->src[ch->pos] - 128;
			sum += (s * ch->volume) >> 8;
			ch->pos++;
		}
		if (sum >  127) sum =  127;
		if (sum < -128) sum = -128;
		out[i] = (unsigned char)(sum + 128);
	}
}

static void reset_channels(void)
{
	memset(channels, 0, sizeof(channels));
}

static int expect_eq_byte(const char *label, unsigned char got, unsigned char want)
{
	if (got == want) {
		printf("ok   %s = 0x%02x\n", label, got);
		return 0;
	}
	printf("FAIL %s: got 0x%02x, want 0x%02x\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;

	/* 1. Single full-volume channel reproduces input. */
	{
		reset_channels();
		unsigned char src[4] = {0x10, 0x80, 0xF0, 0x42};
		channels[0].src = src;
		channels[0].length = 4;
		channels[0].volume = 256;
		channels[0].active = 1;
		unsigned char out[4];
		mixer_fill(out, 4);
		for (int i = 0; i < 4; i++)
			failures += expect_eq_byte("passthrough", out[i], src[i]);
	}

	/* 2. Zero volume = silence rail. */
	{
		reset_channels();
		unsigned char src[2] = {0x00, 0xFF};
		channels[0].src = src;
		channels[0].length = 2;
		channels[0].volume = 0;
		channels[0].active = 1;
		unsigned char out[2];
		mixer_fill(out, 2);
		failures += expect_eq_byte("zero vol byte 0", out[0], 0x80);
		failures += expect_eq_byte("zero vol byte 1", out[1], 0x80);
	}

	/* 3. Eight aligned-peak channels saturate (clip at 0xFF, not wrap). */
	{
		reset_channels();
		static unsigned char loud[1] = {0xFF};
		for (int c = 0; c < MIX_CHANNELS; c++) {
			channels[c].src = loud;
			channels[c].length = 1;
			channels[c].volume = 256;
			channels[c].active = 1;
		}
		unsigned char out[1];
		mixer_fill(out, 1);
		failures += expect_eq_byte("8x peak saturates", out[0], 0xFF);
	}

	/* 4. Two opposing-phase channels cancel to silence. */
	{
		reset_channels();
		static unsigned char high[1] = {0xFF};   /* +127 signed */
		static unsigned char low[1]  = {0x01};   /* -127 signed (close enough) */
		channels[0].src = high; channels[0].length = 1;
		channels[0].volume = 256; channels[0].active = 1;
		channels[1].src = low; channels[1].length = 1;
		channels[1].volume = 256; channels[1].active = 1;
		unsigned char out[1];
		mixer_fill(out, 1);
		failures += expect_eq_byte("opposing cancels", out[0], 0x80);
	}

	/* 5. Channel running past its length deactivates itself. */
	{
		reset_channels();
		unsigned char src[2] = {0x40, 0xC0};
		channels[0].src = src;
		channels[0].length = 2;
		channels[0].volume = 256;
		channels[0].active = 1;
		unsigned char out[4];
		mixer_fill(out, 4);
		failures += expect_eq_byte("autorelease byte 0", out[0], 0x40);
		failures += expect_eq_byte("autorelease byte 1", out[1], 0xC0);
		failures += expect_eq_byte("autorelease byte 2", out[2], 0x80);
		failures += expect_eq_byte("autorelease byte 3", out[3], 0x80);
		if (channels[0].active != 0) {
			printf("FAIL channel did not auto-deactivate\n");
			failures++;
		} else {
			printf("ok   channel auto-deactivates after drain\n");
		}
	}

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
