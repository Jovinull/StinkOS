/* Sound Blaster 16 driver: probe + DSP version readback, ISA I/O at base 0x220.
 *
 * Why SB16: it's the standard "well-documented PCM card" target for hobby OS
 * audio. The DSP speaks a tiny set of single-byte commands over port 0x22C,
 * and ISA DMA channel 1 (8-bit) / channel 5 (16-bit) does the actual data
 * transfer at hardware speed. Newer hardware (AC97, Intel HDA) needs PCI
 * MMIO and scatter-gather descriptor rings -- worth doing eventually but
 * overkill for the first audio commit. QEMU emulates the SB16 faithfully
 * when started with -device sb16. The existing PC-speaker driver coexists;
 * it just can't mix or play PCM, which is what this driver is for. */
#include "audio.h"
#include "dma.h"
#include "io.h"
#include "serial.h"
#include "proc.h"
#include "memlayout.h"
#include "pmm.h"

/* SB16 I/O port layout, base 0x220 (the default; mixer register 0x80 can
 * select 0x240/0x260/0x280 on real hardware, but every common configuration
 * leaves base at 0x220). */
#define SB_BASE          0x220
#define SB_MIXER_INDEX   (SB_BASE + 0x4)
#define SB_MIXER_DATA    (SB_BASE + 0x5)
#define SB_RESET         (SB_BASE + 0x6)
#define SB_READ_DATA     (SB_BASE + 0xA)
#define SB_WRITE_CMD     (SB_BASE + 0xC)   /* bit 7 of read = busy */
#define SB_READ_STATUS   (SB_BASE + 0xE)   /* bit 7 of read = data ready */
#define SB_IRQ_ACK_8BIT  (SB_BASE + 0xE)
#define SB_IRQ_ACK_16BIT (SB_BASE + 0xF)

#define DSP_RESET_RESPONSE 0xAAu
#define DSP_GET_VERSION    0xE1u
#define DSP_SET_RATE       0x41u   /* output sample rate in Hz (HIGH then LOW) */
#define DSP_SPEAKER_ON     0xD1u
#define DSP_SPEAKER_OFF    0xD3u
#define DSP_PAUSE_8BIT     0xD0u
#define DSP_RESUME_8BIT    0xD4u
#define DSP_OUTPUT_8BIT_AI 0xC6u   /* SB16 PIO: 8-bit DAC, auto-init, FIFO */
#define DSP_MODE_MONO_U8   0x00u   /* mono, unsigned 8-bit samples           */

/* 16-bit auto-init equivalents (SB16 only -- earlier SB2/Pro lack the
 * 16-bit DSP path). The mode byte's low nibble picks channel layout +
 * signedness: 0x10 = mono signed, 0x30 = stereo signed. The 16-bit path
 * uses DMA channel 5 (slave controller) and acknowledges via the 16-bit
 * IRQ-ack port (0xF instead of 0xE). Defined here so the eventual
 * switch-over has the constants ready; not yet used by audio_start_output. */
#define DSP_OUTPUT_16BIT_AI 0xB6u   /* 16-bit DAC, auto-init, FIFO          */
#define DSP_PAUSE_16BIT     0xD5u
#define DSP_RESUME_16BIT    0xD6u
#define DSP_MODE_MONO_S16   0x10u   /* mono,   signed 16-bit samples         */
#define DSP_MODE_STEREO_S16 0x30u   /* stereo, signed 16-bit samples         */

/* Audio sample rate for the kernel DMA loop. Doom internally renders at
 * 11025 Hz; 22050 gives headroom for fixed-point mixing without aliasing
 * and is well inside what SB16 handles. */
#define AUDIO_RATE_HZ      22050u

/* DMA ring buffer. Sized + aligned so the chip's address+page register pair
 * can never roll over a 64 KiB boundary mid-transfer: a 16 KiB block aligned
 * to 16 KiB sits at offset 0x0000 / 0x4000 / 0x8000 / 0xC000 within any
 * 64 KiB page, none of which can straddle. Living in kernel BSS keeps it
 * identity-mapped (physical address == virtual). The SB16 fires IRQ5 every
 * AUDIO_HALF_SIZE samples in half-buffer mode, so the mixer refills the
 * half the chip just finished while the other half is still streaming. At
 * 22050 Hz mono u8 this cuts the worst-case audible latency from ~744 ms
 * (full-buffer) to ~372 ms (half-buffer). */
#define AUDIO_BUFFER_SIZE  16384u
#define AUDIO_HALF_SIZE    (AUDIO_BUFFER_SIZE / 2u)

static volatile unsigned char audio_buffer[AUDIO_BUFFER_SIZE]
	__attribute__((aligned(AUDIO_BUFFER_SIZE)));

/* Which half to refill on the next IRQ. The DMA chip plays half 0 first,
 * then half 1, then half 0 again, etc. When the chip finishes half N, it
 * raises IRQ5 and is now playing half (1-N) -- so the safe refill target
 * is half N. */
static volatile int next_fill_half;

/* Set by audio_start_output once the DSP is actually clocking samples. */
static int output_running;

/* Software mixer: up to MIX_CHANNELS concurrent sound effects, each with its
 * own volume and playback cursor. The mixer fills audio_buffer once per IRQ
 * (whole-buffer cycle) by summing every active channel's contribution. Volume
 * is fixed-point 0..256 (256 = unity gain). Samples are mono unsigned 8-bit.
 *
 * Sample ownership. The mixer runs in the SB16 interrupt, with whichever
 * process's address space was loaded when it fired. It therefore never
 * touches user memory: audio_mix_play copies the caller's samples into
 * page frames the channel owns, and the interrupt reads those through the
 * kernel direct map. The caller may free, unmap or exec over its buffer
 * the moment the call returns.
 *
 * Frames are allocated and freed in process context only (the PMM is not
 * interrupt-safe). When the interrupt runs a channel dry it just clears
 * `active`; the frames go back at the next audio_reclaim(), which the
 * timer path calls from a safe point. */
#define MIX_CHANNELS 8
#define MIX_MAX_BYTES  (1u << 20)             /* longest single sound: 1 MiB (~47 s) */
#define MIX_MAX_PAGES  (MIX_MAX_BYTES / 4096u)

struct mix_channel {
	unsigned int         frames[MIX_MAX_PAGES]; /* phys frames holding the copy */
	unsigned int         n_frames;
	unsigned long long   pos;           /* Q48.16 fixed-point index (never wraps in practice) */
	unsigned int         length;        /* in source samples (integer) */
	unsigned int         step;          /* Q16.16 step per output sample */
	int                  volume;        /* 0..256 */
	volatile int         active;        /* set last on play; cleared by the IRQ at the end */
	int                  owner_pid;     /* PID that called audio_mix_play */
	unsigned int         gen;           /* bumped per play: makes stale handles miss */
};

static struct mix_channel channels[MIX_CHANNELS];
static volatile int reclaim_pending;    /* some channel ended and still holds frames */

/* Global master volume (0..256, unity = 256). Applied after the per-channel
 * sum but before saturation, so muting at the master level survives any
 * stale per-channel state and snaps every active sound to silence at once. */
static int master_volume = 256;

/* The reset sequence wants a hold of >= 3 microseconds with the reset line
 * high. Without a known clock, count IO reads on port 0x80 (the legacy
 * "diagnostic port" that's traditionally used as a tiny portable delay). */
#define IO_DELAY_LOOPS     50

/* DSP read/write loops bound their wait so a missing device can't hang the
 * boot. ~100k IO reads is well over a millisecond on any plausible host -- a
 * live DSP responds in microseconds. */
#define DSP_TIMEOUT_LOOPS  100000

static int           sb_present;
static unsigned char dsp_major;
static unsigned char dsp_minor;

/* Bumped on every IRQ5 the SB16 raises. Exposed via audio_irq_count_get for
 * the playback layer (and tests) to confirm DMA is actually cycling. */
static volatile unsigned int irq_count;

static void io_delay(unsigned int loops)
{
	for (unsigned int i = 0; i < loops; i++)
		(void)inb(0x80);
}

/* Pulses the reset line and waits for the DSP to emit the canonical 0xAA
 * acknowledgement byte. Returns 0 on success, -1 if no response arrives
 * within the bounded poll window. */
static int dsp_reset(void)
{
	outb(SB_RESET, 1);
	io_delay(IO_DELAY_LOOPS);
	outb(SB_RESET, 0);

	for (unsigned int spins = 0; spins < DSP_TIMEOUT_LOOPS; spins++) {
		if (!(inb(SB_READ_STATUS) & 0x80))
			continue;
		unsigned char b = inb(SB_READ_DATA);
		return (b == DSP_RESET_RESPONSE) ? 0 : -1;
	}
	return -1;
}

/* DSP commands queue: write port has its bit 7 set while the DSP is still
 * digesting the previous byte. Spin briefly; bail (silently) on timeout so
 * a misbehaving DSP can't lock the kernel. */
static int dsp_write(unsigned char cmd)
{
	for (unsigned int spins = 0; spins < DSP_TIMEOUT_LOOPS; spins++) {
		if (!(inb(SB_WRITE_CMD) & 0x80)) {
			outb(SB_WRITE_CMD, cmd);
			return 0;
		}
	}
	return -1;
}

static int dsp_read(unsigned char *out)
{
	for (unsigned int spins = 0; spins < DSP_TIMEOUT_LOOPS; spins++) {
		if (inb(SB_READ_STATUS) & 0x80) {
			*out = inb(SB_READ_DATA);
			return 0;
		}
	}
	return -1;
}

int audio_init(void)
{
	if (dsp_reset() != 0) {
		serial_write("audio: no SB16 detected at 0x220 "
		             "(start QEMU with -device sb16)\n");
		return 0;
	}

	if (dsp_write(DSP_GET_VERSION) != 0 ||
	    dsp_read(&dsp_major) != 0 ||
	    dsp_read(&dsp_minor) != 0) {
		serial_write("audio: SB16 reset OK but version query failed\n");
		return 0;
	}

	sb_present = 1;
	serial_write("audio: SB16 DSP v");
	serial_write_dec(dsp_major);
	serial_putc('.');
	serial_write_dec(dsp_minor);
	serial_putc('\n');
	return 1;
}

int          audio_present(void)     { return sb_present; }
unsigned int audio_dsp_version(void)
{
	return ((unsigned int)dsp_major << 8) | dsp_minor;
}

/* Fills [offset, offset+n) of audio_buffer with mixed samples from every
 * active mix channel. Per-channel pos/step are Q16.16 fixed-point indices
 * into the source so each channel plays at its own native rate -- the
 * step is (src_rate << 16) / output_rate, computed once at play time.
 * Nearest-sample reads keep the mixer cheap; a real cubic interpolator
 * would clean up the artefacts when ratios are far from unity. */
/* Next sample of an active channel, centred on zero (-128..127). Runs in
 * interrupt context: reads the channel's own frames through the direct
 * map, never user memory. Returns 0 (and retires the channel) at the end
 * of the sound. */
static int ch_pull(struct mix_channel *ch, int *out)
{
	unsigned int idx = (unsigned int)(ch->pos >> 16);
	if (idx >= ch->length) {
		ch->active = 0;                 /* frames are reclaimed in process context */
		reclaim_pending = 1;
		return 0;
	}
	const unsigned char *page = (const unsigned char *)P2V(ch->frames[idx >> 12]);
	*out = (int)page[idx & 0xFFFu] - 128;
	ch->pos += ch->step;
	return 1;
}

static void mixer_fill_window(unsigned int offset, unsigned int n)
{
	unsigned char *out = (unsigned char *)audio_buffer + offset;

	for (unsigned int i = 0; i < n; i++) {
		int sum = 0;
		for (int c = 0; c < MIX_CHANNELS; c++) {
			struct mix_channel *ch = &channels[c];
			if (!ch->active)
				continue;
			int s;
			if (!ch_pull(ch, &s))
				continue;
			sum += (s * ch->volume) >> 8;
		}
		sum = (sum * master_volume) >> 8;
		if (sum >  127) sum =  127;
		if (sum < -128) sum = -128;
		out[i] = (unsigned char)(sum + 128);
	}
}

/* 16-bit signed equivalent of mixer_fill_window. Walks the same channel
 * table the IRQ owns, but expands each per-channel u8-centred sample to
 * the full s16 range (multiply by 256) before summing, then clamps to
 * [-32768, 32767] and stores little-endian into the destination buffer.
 * Used by the SB16 16-bit DMA path when audio_start_output is wired to
 * channel 5; harmless to call early -- nothing reads its output until
 * the playback path flips over. */
static void mixer_fill_window_s16(unsigned char *dst, unsigned int frames)
{
	for (unsigned int i = 0; i < frames; i++) {
		int sum = 0;
		for (int c = 0; c < MIX_CHANNELS; c++) {
			struct mix_channel *ch = &channels[c];
			if (!ch->active)
				continue;
			/* u8 -> s16: scale by 256 to expand the 8-bit dynamic
			 * range into the 16-bit slot. Per-channel volume is
			 * still applied as a 0..256 fixed-point gain. */
			int s;
			if (!ch_pull(ch, &s))
				continue;
			sum += ((s << 8) * ch->volume) >> 8;
		}
		sum = (sum * master_volume) >> 8;
		if (sum >  32767) sum =  32767;
		if (sum < -32768) sum = -32768;
		dst[i * 2 + 0] = (unsigned char)(sum & 0xFF);
		dst[i * 2 + 1] = (unsigned char)((sum >> 8) & 0xFF);
	}
}

void audio_set_master(int volume)
{
	if (volume < 0)   volume = 0;
	if (volume > 256) volume = 256;
	master_volume = volume;
}

int audio_get_master(void)
{
	return master_volume;
}

/* Handles are (generation << 3 | channel): a handle kept after its sound
 * ended does not match the next sound that lands in the same channel. */
#define HANDLE_OF(idx, gen)   ((int)((((gen) & 0x0FFFFFFFu) << 3) | (unsigned int)(idx)))

static void channel_release(struct mix_channel *ch)
{
	for (unsigned int i = 0; i < ch->n_frames; i++)
		pmm_free(ch->frames[i]);
	ch->n_frames = 0;
}

void audio_reclaim(void)
{
	if (!reclaim_pending)
		return;
	reclaim_pending = 0;
	for (int i = 0; i < MIX_CHANNELS; i++)
		if (!channels[i].active && channels[i].n_frames)
			channel_release(&channels[i]);
}

/* The channel a handle refers to, if that sound is still the one in it and
 * the caller owns it; NULL otherwise. */
static struct mix_channel *channel_of(int handle)
{
	if (handle < 0)
		return 0;
	unsigned int idx = (unsigned int)handle & 7u;
	struct mix_channel *ch = &channels[idx];
	if (HANDLE_OF(idx, ch->gen) != handle)
		return 0;
	struct proc *cur = proc_current();
	if (cur && ch->owner_pid != cur->pid)
		return 0;
	return ch;
}

/* Claim the first free mixer channel for a one-shot sound effect and copy
 * `length` unsigned 8-bit samples into it. Volume is clamped to 0..256;
 * pass 256 for unity. Returns a handle, or -1 if every channel is busy,
 * the sound is longer than MIX_MAX_BYTES, or memory ran out.
 * Process context only. */
int audio_mix_play(const unsigned char *samples, unsigned int length, int volume)
{
	if (!samples || length == 0 || length > MIX_MAX_BYTES)
		return -1;
	if (volume < 0)   volume = 0;
	if (volume > 256) volume = 256;

	struct proc *cur = proc_current();
	int pid = cur ? cur->pid : 0;

	audio_reclaim();
	for (int i = 0; i < MIX_CHANNELS; i++) {
		struct mix_channel *ch = &channels[i];
		if (ch->active)
			continue;
		if (ch->n_frames)
			channel_release(ch);

		unsigned int pages = (length + 4095u) / 4096u;
		for (unsigned int pg = 0; pg < pages; pg++) {
			unsigned int frame = pmm_alloc();
			if (!frame) {
				channel_release(ch);
				return -1;
			}
			ch->frames[ch->n_frames++] = frame;
			unsigned char *dst = (unsigned char *)P2V(frame);
			unsigned int off = pg * 4096u;
			unsigned int n = length - off;
			if (n > 4096u)
				n = 4096u;
			for (unsigned int k = 0; k < n; k++)
				dst[k] = samples[off + k];
		}
		ch->pos       = 0;
		ch->length    = length;
		ch->step      = 1u << 16;           /* unity (output_rate) */
		ch->volume    = volume;
		ch->owner_pid = pid;
		ch->gen++;
		ch->active    = 1;                  /* last: the IRQ may pick it up from here */
		return HANDLE_OF(i, ch->gen);
	}
	return -1;
}

int audio_mix_play_rate(const unsigned char *samples, unsigned int length,
                        int volume, unsigned int src_rate)
{
	int h = audio_mix_play(samples, length, volume);
	if (h < 0)
		return -1;
	/* Q16.16 step = src_rate / output_rate. Done in 64-bit so any
	 * realistic source rate (8000 / 11025 / 22050 / 44100 / 48000 / 96000)
	 * computes exactly; we then clamp the resulting step to [1, 16x] so a
	 * pathological caller cannot overflow the index in a few samples.
	 *
	 * Pre-§13 this helper was 32-bit-clamped (src_rate <= 65535) to dodge
	 * a libgcc __udivdi3 dependency that would have pushed the kernel past
	 * the bootloader's 64 KiB cap. Now that the ELF-aware bootloader is in
	 * place, the kernel can grow freely; the 64-bit form below is the
	 * straightforward computation. */
	if (src_rate == 0)
		src_rate = AUDIO_RATE_HZ;
	unsigned int step = (unsigned int)(((unsigned long long)src_rate << 16) / AUDIO_RATE_HZ);
	if (step == 0) step = 1;
	if (step > (16u << 16)) step = (16u << 16);   /* 16x speed ceiling */
	channels[h & 7].step = step;
	return h;
}

void audio_mix_stop(int handle)
{
	struct mix_channel *ch = channel_of(handle);
	if (!ch)
		return;
	ch->active = 0;
	channel_release(ch);
}

void audio_mix_set_volume(int handle, int volume)
{
	struct mix_channel *ch = channel_of(handle);
	if (!ch)
		return;
	if (volume < 0)   volume = 0;
	if (volume > 256) volume = 256;
	ch->volume = volume;
}

int audio_mix_playing(int handle)
{
	struct mix_channel *ch = channel_of(handle);
	return ch && ch->active;
}

/* Silence every channel without touching the DSP. */
void audio_mix_silence_all(void)
{
	for (int i = 0; i < MIX_CHANNELS; i++) {
		channels[i].active = 0;
		channel_release(&channels[i]);
	}
}

/* Silence only the channels owned by `pid` and give their sample frames
 * back. Called when a process exits, is killed or is suspended. A `pid`
 * of 0 matches the kernel boot context. */
void audio_mix_silence_pid(int pid)
{
	for (int i = 0; i < MIX_CHANNELS; i++) {
		struct mix_channel *ch = &channels[i];
		if (ch->owner_pid != pid || (!ch->active && !ch->n_frames))
			continue;
		ch->active = 0;
		channel_release(ch);
	}
}

/* Stereo s16 mixer. Sources are mono u8 -- the same dynamic-range
 * expansion as mixer_fill_window_s16, then the result is written to
 * BOTH channels of each interleaved L/R frame. Future per-channel
 * panning would multiply L and R by separate gains here; today they
 * just mirror. Buffer holds frames * 4 bytes (2 channels * 2 bytes). */
static void mixer_fill_window_s16_stereo(unsigned char *dst, unsigned int frames)
{
	for (unsigned int i = 0; i < frames; i++) {
		int sum = 0;
		for (int c = 0; c < MIX_CHANNELS; c++) {
			struct mix_channel *ch = &channels[c];
			if (!ch->active)
				continue;
			int s;
			if (!ch_pull(ch, &s))
				continue;
			sum += ((s << 8) * ch->volume) >> 8;
		}
		sum = (sum * master_volume) >> 8;
		if (sum >  32767) sum =  32767;
		if (sum < -32768) sum = -32768;
		/* L sample, then R sample -- SB16 stereo expects interleaved. */
		dst[i * 4 + 0] = (unsigned char)(sum & 0xFF);
		dst[i * 4 + 1] = (unsigned char)((sum >> 8) & 0xFF);
		dst[i * 4 + 2] = (unsigned char)(sum & 0xFF);
		dst[i * 4 + 3] = (unsigned char)((sum >> 8) & 0xFF);
	}
}

/* Output mode selector for audio_handle_irq:
 *   0 = mono u8   (mixer_fill_window)
 *   1 = mono s16  (mixer_fill_window_s16)
 *   2 = stereo s16 (mixer_fill_window_s16_stereo) */
#define AUDIO_MODE_MONO_U8     0
#define AUDIO_MODE_MONO_S16    1
#define AUDIO_MODE_STEREO_S16  2
static int output_is_16bit;     /* legacy name: now holds AUDIO_MODE_* value */

/* IRQ5 dispatcher: the SB16 raises this every half-buffer in auto-init mode,
 * so we refill the half that just finished playing (next_fill_half) while
 * the chip streams the other half. Toggling the flag keeps the two halves
 * ping-ponging without us having to read DMA progress registers. */
void audio_handle_irq(void)
{
	irq_count++;
	unsigned char *dst = (unsigned char *)audio_buffer +
	                     next_fill_half * AUDIO_HALF_SIZE;
	switch (output_is_16bit) {
	case AUDIO_MODE_MONO_S16:
		/* 2 bytes per frame */
		mixer_fill_window_s16(dst, AUDIO_HALF_SIZE / 2u);
		break;
	case AUDIO_MODE_STEREO_S16:
		/* 4 bytes per frame (L + R, s16 each) */
		mixer_fill_window_s16_stereo(dst, AUDIO_HALF_SIZE / 4u);
		break;
	default:
		mixer_fill_window(next_fill_half * AUDIO_HALF_SIZE, AUDIO_HALF_SIZE);
		break;
	}
	next_fill_half ^= 1;
	(void)inb(SB_BASE + 0xE);              /* 8-bit DMA ack */
	(void)inb(SB_BASE + 0xF);              /* 16-bit DMA ack */
}

unsigned int audio_irq_count_get(void) { return irq_count; }

int audio_current_mode(void)
{
	if (!output_running)
		return -1;
	return output_is_16bit;     /* holds the AUDIO_MODE_* enum value */
}

/* Fill the whole DMA ring with one byte value. Silence is 0x80 for
 * unsigned 8-bit output and 0x00 for signed 16-bit; whatever the previous
 * format (or the BSS) left in the ring would otherwise play as a
 * full-scale burst until both halves have been refilled. */
static void ring_fill(unsigned char v)
{
	unsigned char *b = (unsigned char *)audio_buffer;
	for (unsigned int i = 0; i < AUDIO_BUFFER_SIZE; i++)
		b[i] = v;
}

/* SB16 sample-rate command: two-byte HIGH-then-LOW big-endian rate. */
static void dsp_set_output_rate(unsigned int hz)
{
	dsp_write(DSP_SET_RATE);
	dsp_write((unsigned char)((hz >> 8) & 0xFF));
	dsp_write((unsigned char)(hz & 0xFF));
}

/* Start an auto-init 8-bit mono PCM stream at AUDIO_RATE_HZ. The DMA chip
 * walks audio_buffer endlessly; when the count register hits 0 the SB16
 * fires IRQ5 and (because of auto-init) immediately restarts at the buffer
 * base. Anything written into audio_buffer will play; the mixer fills it. */
int audio_start_output(void)
{
	if (!sb_present)
		return -1;
	if (output_running)
		return 0;

	dma_channel1_program(V2P((unsigned int)audio_buffer),
	                     AUDIO_BUFFER_SIZE,
	                     DMA_MODE_SINGLE | DMA_MODE_AUTOINIT | DMA_MODE_READ);

	dsp_write(DSP_SPEAKER_ON);
	dsp_set_output_rate(AUDIO_RATE_HZ);

	/* Program the BLOCK size, not the buffer size: when the chip plays
	 * `block` samples in auto-init it fires IRQ5 and rewinds inside the
	 * DMA ring. Setting block = half-buffer gives twice-per-cycle IRQs,
	 * halving worst-case mixer latency. */
	unsigned int count = AUDIO_HALF_SIZE - 1;
	dsp_write(DSP_OUTPUT_8BIT_AI);
	dsp_write(DSP_MODE_MONO_U8);
	dsp_write((unsigned char)(count & 0xFF));
	dsp_write((unsigned char)((count >> 8) & 0xFF));

	ring_fill(0x80);                        /* start from silence in this format */
	next_fill_half = 0;
	output_is_16bit = AUDIO_MODE_MONO_U8;
	output_running  = 1;
	serial_write("audio: SB16 output started, 22050 Hz mono u8, half-buffer IRQs\n");
	return 0;
}

/* Auto-init signed 16-bit mono PCM via DMA channel 5 + DSP command
 * 0xB6. Same half-buffer ping-pong as the 8-bit path; the IRQ branches
 * on output_is_16bit to pick mixer_fill_window_s16. Buffer holds half
 * as many samples (2 bytes each) but the same wall-time at the same
 * sample rate. Either start function can be called; the most recent
 * wins, but tearing down + re-arming the DSP between modes is the
 * caller's job. */
int audio_start_output_16bit(void)
{
	if (!sb_present)
		return -1;
	if (output_running)
		return 0;

	dma_channel5_program(V2P((unsigned int)audio_buffer),
	                     AUDIO_BUFFER_SIZE,
	                     DMA_MODE_SINGLE | DMA_MODE_AUTOINIT | DMA_MODE_READ);

	dsp_write(DSP_SPEAKER_ON);
	dsp_set_output_rate(AUDIO_RATE_HZ);

	/* 16-bit DSP block size is in SAMPLES, not bytes. AUDIO_HALF_SIZE
	 * bytes = AUDIO_HALF_SIZE/2 frames in mono s16. */
	unsigned int count = (AUDIO_HALF_SIZE / 2u) - 1u;
	dsp_write(DSP_OUTPUT_16BIT_AI);
	dsp_write(DSP_MODE_MONO_S16);
	dsp_write((unsigned char)(count & 0xFF));
	dsp_write((unsigned char)((count >> 8) & 0xFF));

	ring_fill(0x00);                        /* start from silence in this format */
	next_fill_half = 0;
	output_is_16bit = AUDIO_MODE_MONO_S16;
	output_running  = 1;
	serial_write("audio: SB16 output started, 22050 Hz mono s16, half-buffer IRQs\n");
	return 0;
}

/* Stereo signed-16 variant. Same DMA channel 5 wiring as mono s16, only
 * the DSP mode byte changes to 0x30 and the IRQ uses the stereo mixer
 * that writes 4 bytes per frame instead of 2. */
int audio_start_output_stereo(void)
{
	if (!sb_present)
		return -1;
	if (output_running)
		return 0;

	dma_channel5_program(V2P((unsigned int)audio_buffer),
	                     AUDIO_BUFFER_SIZE,
	                     DMA_MODE_SINGLE | DMA_MODE_AUTOINIT | DMA_MODE_READ);

	dsp_write(DSP_SPEAKER_ON);
	dsp_set_output_rate(AUDIO_RATE_HZ);

	/* The DSP block length counts 16-bit samples, stereo or not: a
	 * half-buffer of AUDIO_HALF_SIZE bytes is AUDIO_HALF_SIZE / 2 of
	 * them. Dividing by the frame size (4) made the card interrupt every
	 * quarter buffer while the handler refilled a half, so every sound
	 * ran at double speed with alternate chunks dropped. */
	unsigned int count = (AUDIO_HALF_SIZE / 2u) - 1u;
	dsp_write(DSP_OUTPUT_16BIT_AI);
	dsp_write(DSP_MODE_STEREO_S16);
	dsp_write((unsigned char)(count & 0xFF));
	dsp_write((unsigned char)((count >> 8) & 0xFF));

	ring_fill(0x00);                        /* start from silence in this format */
	next_fill_half = 0;
	output_is_16bit = AUDIO_MODE_STEREO_S16;
	output_running  = 1;
	serial_write("audio: SB16 output started, 22050 Hz stereo s16, half-buffer IRQs\n");
	return 0;
}

void audio_stop_output(void)
{
	if (!output_running)
		return;
	dsp_write(DSP_PAUSE_8BIT);
	dsp_write(DSP_SPEAKER_OFF);
	output_running = 0;
}

/* Returns the kernel-side DMA buffer the SB16 is currently streaming, plus
 * its size. The mixer writes mixed samples here every IRQ tick. */
unsigned char *audio_buffer_ptr(void) { return (unsigned char *)audio_buffer; }
unsigned int   audio_buffer_size(void) { return AUDIO_BUFFER_SIZE; }
unsigned int   audio_sample_rate(void) { return AUDIO_RATE_HZ; }
