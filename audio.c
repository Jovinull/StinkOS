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
#include "io.h"
#include "serial.h"

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

/* IRQ5 dispatcher: the SB16 raises this when a DMA half-buffer or full-
 * buffer completion is reached. We acknowledge both 8-bit and 16-bit on
 * every fire -- two cheap port reads, and we don't yet care which side
 * triggered. The mixer interrupt-status register (port 0x224 idx 0x82) can
 * tell us "DMA8 / DMA16 / MPU" if we ever need to disambiguate. */
void audio_handle_irq(void)
{
	irq_count++;
	(void)inb(SB_BASE + 0xE);              /* 8-bit DMA ack */
	(void)inb(SB_BASE + 0xF);              /* 16-bit DMA ack */
}

unsigned int audio_irq_count_get(void) { return irq_count; }
