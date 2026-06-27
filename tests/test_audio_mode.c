/* Host-side test for the audio mode selector in kernel/drivers/audio/audio.c
 * (SYS_AUDIO_MODE + SYS_AUDIO_QUERY). The kernel keeps a single int
 * `output_is_16bit` whose values are the AUDIO_MODE_* enum -- not just
 * 0/1. audio_current_mode returns it (or -1 if no playback armed).
 *
 * Failure mode this guards: silently advancing to 16-bit while the IRQ
 * still thinks it's u8 (or vice versa) would feed the SB16 the wrong
 * bytes-per-sample math and either corrupt audio or run the IRQ off
 * the end of the ring buffer.
 */
#include <stdio.h>

#define AUDIO_MODE_MONO_U8     0
#define AUDIO_MODE_MONO_S16    1
#define AUDIO_MODE_STEREO_S16  2

static int sb_present;
static int output_running;
static int output_is_16bit;

static void sim_reset(void)
{
	sb_present     = 1;
	output_running = 0;
	output_is_16bit = AUDIO_MODE_MONO_U8;
}

static int sim_start_u8(void)
{
	if (!sb_present) return -1;
	if (output_running) return 0;
	output_is_16bit = AUDIO_MODE_MONO_U8;
	output_running  = 1;
	return 0;
}

static int sim_start_s16(void)
{
	if (!sb_present) return -1;
	if (output_running) return 0;
	output_is_16bit = AUDIO_MODE_MONO_S16;
	output_running  = 1;
	return 0;
}

static int sim_start_stereo(void)
{
	if (!sb_present) return -1;
	if (output_running) return 0;
	output_is_16bit = AUDIO_MODE_STEREO_S16;
	output_running  = 1;
	return 0;
}

static void sim_stop(void)
{
	output_running = 0;
}

static int sim_current_mode(void)
{
	if (!output_running) return -1;
	return output_is_16bit;
}

/* Mirrors the SYS_AUDIO_MODE syscall: stop, then start the requested
 * variant, return rc. */
static int sim_audio_mode(int mode)
{
	sim_stop();
	switch (mode) {
	case 0: return sim_start_u8();
	case 1: return sim_start_s16();
	case 2: return sim_start_stereo();
	default: return -1;
	}
}

static int expect_int(const char *label, int got, int want)
{
	if (got == want) { printf("ok   %-50s = %d\n", label, got); return 0; }
	printf("FAIL %s: got %d, want %d\n", label, got, want);
	return 1;
}

int main(void)
{
	int failures = 0;

	/* No playback yet -- query returns -1. */
	sim_reset();
	failures += expect_int("idle: query = -1",            sim_current_mode(), -1);

	/* Start u8 -- query reflects. */
	failures += expect_int("start u8: rc = 0",            sim_audio_mode(0), 0);
	failures += expect_int("query after u8 = 0",          sim_current_mode(), 0);

	/* Switch to s16 -- stop+start cycle preserved. */
	failures += expect_int("start s16: rc = 0",           sim_audio_mode(1), 0);
	failures += expect_int("query after s16 = 1",         sim_current_mode(), 1);

	/* Switch to stereo. */
	failures += expect_int("start stereo: rc = 0",        sim_audio_mode(2), 0);
	failures += expect_int("query after stereo = 2",      sim_current_mode(), 2);

	/* Unknown mode rejected -- the previous mode stays armed only if
	 * stop() was a no-op... in our impl stop runs first regardless, so
	 * after a bad mode call we're stopped: query = -1. */
	failures += expect_int("bad mode 99: rc = -1",        sim_audio_mode(99), -1);
	failures += expect_int("query after bad mode = -1",   sim_current_mode(), -1);

	/* sb_present = 0: every start returns -1. */
	sim_reset();
	sb_present = 0;
	failures += expect_int("no SB16: u8 start rc = -1",      sim_audio_mode(0), -1);
	failures += expect_int("no SB16: query = -1",            sim_current_mode(), -1);

	/* Double-start is idempotent (returns 0 without modifying). */
	sim_reset();
	sim_audio_mode(1);
	output_running = 1;             /* simulate already-running */
	int prev = output_is_16bit;
	int rc = sim_start_s16();
	failures += expect_int("double-start s16 returns 0",     rc, 0);
	failures += expect_int("double-start: mode unchanged",   output_is_16bit, prev);

	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
