/* 8237 master DMA controller programming, channel 1 only.
 *
 * The 7-step canonical setup sequence (Intel application note 8237A):
 *   1. Mask the channel so it can't fire mid-program.
 *   2. Clear the byte-pointer flip-flop (so address/count writes hit "low").
 *   3. Write the mode register (direction + auto-init + channel).
 *   4. Write address low then high (each byte advances the flip-flop).
 *   5. Write the page register (high 8 bits of the 24-bit physical addr).
 *   6. Write count low then high (count = bytes - 1 for 8-bit channels).
 *   7. Unmask the channel; the device's next DRQ fires the transfer.
 */
#include "dma.h"
#include "io.h"

/* Master controller port layout. (The slave controller for 16-bit channels
 * lives at 0xC0-0xDF with 2x scaling -- not handled here.) */
#define DMA1_MASK            0x0A   /* write: |0x04 = mask, else unmask    */
#define DMA1_MODE            0x0B
#define DMA1_CLEAR_FF        0x0C

#define DMA1_CHAN1_ADDR      0x02   /* low / high alternates via flip-flop */
#define DMA1_CHAN1_COUNT     0x03
#define DMA1_CHAN1_PAGE      0x83   /* bits 23..16 of the physical address */

#define CHANNEL_MASK_BIT     0x04

void dma_channel1_program(unsigned int phys_addr,
                          unsigned int count_bytes,
                          unsigned int mode)
{
	/* Hardware wants count - 1, in bytes, expressed in 16 bits. */
	unsigned int count = (count_bytes - 1) & 0xFFFFu;

	outb(DMA1_MASK,     CHANNEL_MASK_BIT | 0x01);   /* mask channel 1   */
	outb(DMA1_CLEAR_FF, 0x00);                      /* reset flip-flop  */
	outb(DMA1_MODE,     (unsigned char)(mode | 0x01)); /* mode + chan 1 */

	outb(DMA1_CHAN1_ADDR,  phys_addr        & 0xFF);
	outb(DMA1_CHAN1_ADDR, (phys_addr >> 8)  & 0xFF);
	outb(DMA1_CHAN1_PAGE, (phys_addr >> 16) & 0xFF);

	outb(DMA1_CHAN1_COUNT,  count       & 0xFF);
	outb(DMA1_CHAN1_COUNT, (count >> 8) & 0xFF);

	outb(DMA1_MASK, 0x01);                          /* unmask channel 1 */
}

/* Slave (16-bit) controller layout. Ports are 2x scaled from the master,
 * and the address/count registers count WORDS, not bytes -- so the byte
 * address is shifted right by 1 before splitting into low+high, and the
 * byte count is divided by 2 before writing. The slave page register for
 * channel 5 lives at port 0x8B (one of the legacy "stale" page slots that
 * the OEM PC designers reused for the slave). */
#define DMA2_MASK            0xD4   /* slave mask register              */
#define DMA2_MODE            0xD6
#define DMA2_CLEAR_FF        0xD8

#define DMA2_CHAN5_ADDR      0xC4   /* word address, low/high via flip-flop */
#define DMA2_CHAN5_COUNT     0xC6   /* word count, low/high via flip-flop   */
#define DMA2_CHAN5_PAGE      0x8B   /* bits 23..17 of the 24-bit phys addr  */

void dma_channel5_program(unsigned int phys_addr,
                          unsigned int count_bytes,
                          unsigned int mode)
{
	unsigned int word_addr  = phys_addr  >> 1;
	unsigned int word_count = (count_bytes >> 1) - 1u;
	word_count &= 0xFFFFu;

	/* Mask channel 5 (low 2 bits of the slave's mask register select the
	 * channel within that controller -- channel 5 is slave-relative 1). */
	outb(DMA2_MASK,     CHANNEL_MASK_BIT | 0x01);
	outb(DMA2_CLEAR_FF, 0x00);
	outb(DMA2_MODE,     (unsigned char)(mode | 0x01));   /* mode + chan 5 */

	outb(DMA2_CHAN5_ADDR,  word_addr        & 0xFF);
	outb(DMA2_CHAN5_ADDR, (word_addr >> 8)  & 0xFF);
	outb(DMA2_CHAN5_PAGE, (phys_addr >> 16) & 0xFE);     /* bit 0 reserved */

	outb(DMA2_CHAN5_COUNT,  word_count       & 0xFF);
	outb(DMA2_CHAN5_COUNT, (word_count >> 8) & 0xFF);

	outb(DMA2_MASK, 0x01);                              /* unmask channel 5 */
}
