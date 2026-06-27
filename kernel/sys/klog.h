/* In-memory kernel log ring buffer.
 *
 * Every byte written to the serial console (`serial_putc`) is mirrored
 * into a fixed-size circular buffer. The buffer survives any boot phase
 * past serial_init -- the very first serial_putc lands in slot 0 and
 * wraps from there. Userland reads it via SYS_KLOG_READ; the `dmesg`
 * shell command is just a thin wrapper.
 *
 * Size: 8 KiB. Enough for the entire boot diagnostic stream plus a few
 * minutes of TCP / fs / pmm traffic; old bytes get dropped silently when
 * the ring wraps. This is purely a debugging aid -- the source of truth
 * is still the serial line, which never drops anything.
 */
#ifndef KLOG_H
#define KLOG_H

#define KLOG_SIZE  8192u

/* Append one byte to the ring. Called from serial_putc. */
void klog_push(char c);

/* Copy up to `cap` bytes of the most-recent ring contents into `out`,
 * oldest first. Returns the byte count actually written. If the ring has
 * wrapped, only the last `KLOG_SIZE` bytes are preserved; older data is
 * gone. */
unsigned int klog_read(char *out, unsigned int cap);

#endif
