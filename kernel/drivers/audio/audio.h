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

/* IRQ5 dispatch. Called from the kernel's IRQ handler when the SB16 raises
 * an interrupt (DMA half/full buffer completion). Reads the ack ports and
 * advances internal state. */
void audio_handle_irq(void);

/* How many times audio_handle_irq has been invoked since boot. Tests + the
 * mixer use this to verify the DSP is actually clocking through DMA. */
unsigned int audio_irq_count_get(void);

/* Start the SB16's auto-init DMA loop, streaming whatever sits in the kernel
 * audio ring buffer at AUDIO_RATE_HZ mono unsigned 8-bit. Returns 0 on
 * success, -1 if no DSP is present. Idempotent (no-op when already running). */
int  audio_start_output(void);

/* Same contract as audio_start_output but uses DMA channel 5 + DSP 0xB6
 * to clock mono signed 16-bit samples instead of mono u8. The mixer
 * automatically widens its u8 sources to the s16 dynamic range. Pick
 * one and stick with it for the lifetime of the boot; mode-switching is
 * not supported without a full audio_stop_output + re-init dance. */
int  audio_start_output_16bit(void);

/* Stereo signed-16 output -- DMA channel 5 + DSP mode 0x30. Each mono
 * source is duplicated into both L and R channels until per-channel
 * panning lands. Same wall-time as the mono variants at the same
 * sample rate; the DMA ring just holds half as many frames because
 * each one is 4 bytes (vs 2 for mono s16, or 1 for u8). */
int  audio_start_output_stereo(void);

/* Pause the DSP + cut the speaker. The DMA channel remains armed so a
 * subsequent audio_start_output picks back up cleanly. */
void audio_stop_output(void);

/* DMA ring buffer accessors. The mixer fills audio_buffer_ptr()[0..size)
 * with mixed mono 8-bit unsigned samples; the SB16 reads them in lockstep. */
unsigned char *audio_buffer_ptr(void);
unsigned int   audio_buffer_size(void);
unsigned int   audio_sample_rate(void);

/* Software mixer. Up to 8 concurrent channels; each takes a pointer to mono
 * unsigned 8-bit samples plus a length and a 0..256 volume (256 = unity).
 * The source pointer is NOT copied -- the caller is responsible for keeping
 * the sample buffer alive until the channel drains or audio_mix_stop is
 * called. audio_mix_silence_all clears every channel without touching the
 * DSP and is what menu_exit uses before unmapping the app's address space. */
int  audio_mix_play(const unsigned char *samples, unsigned int length, int volume);

/* Play `samples` at `src_rate` (Hz). Computes a Q16.16 step into the source
 * so the kernel mixer resamples on the fly via nearest neighbour. Pass 0
 * for src_rate to mean "same as output", which is what audio_mix_play
 * implicitly assumes. Returns the channel handle or -1 if no slot is free. */
int  audio_mix_play_rate(const unsigned char *samples, unsigned int length,
                         int volume, unsigned int src_rate);
void audio_mix_stop(int handle);
void audio_mix_set_volume(int handle, int volume);
void audio_mix_silence_all(void);

/* Silence channels owned by `pid` only. Per-channel ownership is set at
 * audio_mix_play time from proc_current(); this lets the scheduler stop a
 * suspended or exiting process's audio without disturbing peers. */
void audio_mix_silence_pid(int pid);

/* Master volume: scales the final mixer output before it lands in the DMA
 * ring. Range 0..256 (256 = unity). One scalar applied to every channel,
 * so apps can fade or mute the whole soundscape without walking each
 * per-channel volume. */
void audio_set_master(int volume);
int  audio_get_master(void);

#endif
