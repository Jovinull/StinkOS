/* PC speaker driver. PIT channel 2 runs in square-wave mode at the requested
 * frequency; port 0x61 bits 0 (gate) and 1 (data) connect that output to the
 * speaker. Silencing just clears those two bits and leaves the timer alone. */
#include "speaker.h"
#include "io.h"

#define PIT_HZ      1193180u       /* PIT input clock */
#define PIT_CH2     0x42
#define PIT_CMD     0x43
#define SPK_PORT    0x61

void speaker_play(unsigned int freq)
{
	if (freq == 0) {                                  /* silence */
		outb(SPK_PORT, inb(SPK_PORT) & 0xFC);
		return;
	}

	unsigned int divisor = PIT_HZ / freq;

	outb(PIT_CMD, 0xB6);                              /* ch2, mode 3, lo/hi byte */
	outb(PIT_CH2, (unsigned char)(divisor & 0xFF));
	outb(PIT_CH2, (unsigned char)((divisor >> 8) & 0xFF));

	unsigned char gate = inb(SPK_PORT);
	if ((gate & 0x03) != 0x03)                        /* connect speaker to ch2 */
		outb(SPK_PORT, gate | 0x03);
}
