/* ISA DMA (8237) controller programming. The PCs of 1981-1995 used two
 * cascaded 8237 chips: the master serves channels 0-3 (8-bit transfers), the
 * slave serves channels 4-7 (16-bit, with channel 4 reserved for cascade).
 *
 * StinkOS only needs channel 1 today -- that's the channel SB16 uses for
 * 8-bit audio output. Other channels (2 = floppy historically, 5 = SB16
 * 16-bit) get their own helpers when something actually wants them.
 *
 * Caller responsibilities for ISA DMA buffers:
 *   - Physically contiguous (PMM frames are 4 KiB; arrange a 16 KiB-aligned
 *     16 KiB static buffer in kernel BSS and the chunk is guaranteed
 *     contiguous because the kernel is identity-mapped in low memory).
 *   - Must not cross a 64 KiB page boundary -- the 8237 walks a 16-bit
 *     offset inside a fixed 8-bit page register, so it can't roll over.
 *   - Below 16 MiB physical (24-bit address bus on the 8237). Our PMM
 *     manages 1-32 MiB, so frames live below this limit by construction.
 */
#ifndef DMA_H
#define DMA_H

/* 8237 mode-register fields. OR them together to compose the mode byte
 * passed to dma_channel1_program. The channel-number bits are filled in by
 * the helper -- callers never set them. */
#define DMA_MODE_VERIFY      0x00
#define DMA_MODE_WRITE       0x04   /* device -> memory: input from device  */
#define DMA_MODE_READ        0x08   /* memory -> device: output to device   */
#define DMA_MODE_AUTOINIT    0x10   /* reload count + addr when count = 0   */
#define DMA_MODE_DECREMENT   0x20   /* walk addresses downward              */
#define DMA_MODE_DEMAND      0x00   /* transfer while device asserts DRQ    */
#define DMA_MODE_SINGLE      0x40   /* one byte per DRQ assertion           */
#define DMA_MODE_BLOCK       0x80   /* transfer whole block once DRQ asserts*/

/* Program channel 1 (master controller) for a transfer. The physical address
 * must satisfy the alignment rules above; count_bytes is the raw byte count
 * (the helper sends count - 1 to the chip, as the hardware expects). 'mode'
 * picks direction + auto-init + transfer mode. Returns nothing -- failure
 * modes are programmer error, not runtime conditions. */
void dma_channel1_program(unsigned int phys_addr,
                          unsigned int count_bytes,
                          unsigned int mode);

/* Program channel 5 (slave controller, 16-bit) for an SB16 16-bit audio
 * transfer. The slave's address + count registers count in WORDS, not
 * bytes, and the address+page split is at a 17-bit boundary (the page
 * register holds bits 23..17). The buffer therefore must be aligned to
 * 64 KiB worst case AND must not cross a 128 KiB boundary mid-transfer.
 * count_bytes still expresses the BYTE count for the caller's
 * convenience; the helper translates internally. Used by the SB16
 * 16-bit playback path when it lands; harmless to call early. */
void dma_channel5_program(unsigned int phys_addr,
                          unsigned int count_bytes,
                          unsigned int mode);

#endif
