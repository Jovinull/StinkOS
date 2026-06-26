/* Audio subsystem: Sound Blaster 16 driver on the ISA bus. The kernel-side
 * surface starts at probe -- detect whether the DSP exists, log its firmware
 * version -- and grows toward DMA playback + a software mixer as the rest of
 * the stack lands. apps see only the libstink wrappers; the SB-specific
 * register layout never crosses into userland. */
#ifndef AUDIO_H
#define AUDIO_H

/* Probe the SB16 DSP at the default I/O base (0x220). Returns 1 when a DSP
 * responds to the reset sequence, 0 when nothing is wired up (no -device sb16
 * on the QEMU command line, real hardware lacking the card, etc.). Logs the
 * detected version on the serial console either way. Safe to call exactly
 * once during kernel boot. */
int audio_init(void);

/* Returns 1 if audio_init() succeeded, 0 otherwise. Drivers that produce
 * sound should bail out early when this returns 0 instead of poking I/O
 * ports that aren't backed by a real device. */
int audio_present(void);

/* DSP firmware version as (major << 8) | minor, or 0 when absent. QEMU's
 * SB16 emulation reports 4.5; real cards range from 1.0 (SB Pro) up to 4.x
 * (SB16/AWE32). */
unsigned int audio_dsp_version(void);

#endif
