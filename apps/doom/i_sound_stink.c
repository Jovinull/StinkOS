/* StinkOS sound backend for doomgeneric. Wires Doom's sound_module_t interface
 * onto the kernel mixer reachable via sys_audio_play/stop/set_volume. Sound
 * effects are pulled from WAD lumps (DSxxx format: 8-byte header followed by
 * mono unsigned 8-bit samples), upsampled to the mixer's 22050 Hz output via
 * a simple 2x duplication, and handed off to a free mixer channel.
 *
 * Music: stubbed. Real MIDI synthesis needs an OPL or wavetable backend that
 * this OS doesn't have yet; the stub keeps the rest of the engine happy.
 */
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "m_misc.h"
#include "deh_str.h"

#include "libstink.h"

#define STINK_CHANNELS  8

typedef struct {
	int            kernel_handle;
	unsigned char *upsampled;
	int            active;
} stink_chan_t;

static stink_chan_t  stink_channels[STINK_CHANNELS];
static snddevice_t   stink_sfx_devices[]   = { SNDDEVICE_SB };
static snddevice_t   stink_music_devices[] = { SNDDEVICE_SB };

/* ---- sfx module ---- */

static boolean Stink_SfxInit(boolean use_sfx_prefix)
{
	(void)use_sfx_prefix;
	for (int i = 0; i < STINK_CHANNELS; i++) {
		stink_channels[i].kernel_handle = -1;
		stink_channels[i].upsampled     = (unsigned char *)0;
		stink_channels[i].active        = 0;
	}
	return true;
}

static void Stink_SfxShutdown(void) {}

static int Stink_GetSfxLumpNum(sfxinfo_t *sfx)
{
	char namebuf[9];
	M_snprintf(namebuf, sizeof(namebuf), "ds%s", DEH_String(sfx->name));
	return W_GetNumForName(namebuf);
}

static void Stink_SfxUpdate(void)
{
	/* The kernel mixer self-drains exhausted channels; nothing periodic to
	 * do from userland. */
}

static void Stink_SfxUpdateParams(int channel, int vol, int sep)
{
	(void)sep;        /* mono mixer for now */
	if (channel < 0 || channel >= STINK_CHANNELS || !stink_channels[channel].active)
		return;
	/* Doom volumes are 0..127; kernel mixer takes 0..256. */
	sys_audio_set_volume(stink_channels[channel].kernel_handle, vol * 2);
}

static void Stink_ReleaseSlot(int channel)
{
	if (stink_channels[channel].active) {
		sys_audio_stop(stink_channels[channel].kernel_handle);
	}
	if (stink_channels[channel].upsampled) {
		free(stink_channels[channel].upsampled);
		stink_channels[channel].upsampled = (unsigned char *)0;
	}
	stink_channels[channel].active = 0;
	stink_channels[channel].kernel_handle = -1;
}

static int Stink_StartSound(sfxinfo_t *sfx, int channel, int vol, int sep)
{
	(void)sep;
	if (channel < 0 || channel >= STINK_CHANNELS)
		return -1;

	Stink_ReleaseSlot(channel);

	unsigned char *lump = W_CacheLumpNum(sfx->lumpnum, PU_STATIC);
	int lump_len = W_LumpLength(sfx->lumpnum);
	if (lump_len < 8) {
		W_ReleaseLumpNum(sfx->lumpnum);
		return -1;
	}

	/* DMX sound lump header: u16 format, u16 sample rate, u32 num_samples,
	 * then samples. Format 3 = unsigned 8-bit PCM, which every vanilla WAD
	 * uses; rate is typically 11025 Hz. */
	unsigned int num_samples =
	    (unsigned int)lump[4] |
	    ((unsigned int)lump[5] << 8) |
	    ((unsigned int)lump[6] << 16) |
	    ((unsigned int)lump[7] << 24);
	if (num_samples + 8 > (unsigned int)lump_len)
		num_samples = (unsigned int)lump_len - 8;

	const unsigned char *src = lump + 8;

	/* Upsample 11025 -> 22050 by duplicating each sample. Wastes CPU + RAM
	 * compared to fixed-point step in the mixer, but it's simple and keeps
	 * the kernel side at one playback rate. Refactor when latency matters. */
	unsigned int up_len = num_samples * 2;
	unsigned char *up = malloc(up_len);
	if (!up) {
		W_ReleaseLumpNum(sfx->lumpnum);
		return -1;
	}
	for (unsigned int i = 0; i < num_samples; i++) {
		up[i * 2]     = src[i];
		up[i * 2 + 1] = src[i];
	}
	W_ReleaseLumpNum(sfx->lumpnum);

	int handle = sys_audio_play(up, up_len, vol * 2);
	if (handle < 0) {
		free(up);
		return -1;
	}

	stink_channels[channel].kernel_handle = handle;
	stink_channels[channel].upsampled     = up;
	stink_channels[channel].active        = 1;
	return channel;
}

static void Stink_StopSound(int channel)
{
	if (channel < 0 || channel >= STINK_CHANNELS)
		return;
	Stink_ReleaseSlot(channel);
}

static boolean Stink_SoundIsPlaying(int channel)
{
	if (channel < 0 || channel >= STINK_CHANNELS)
		return false;
	return stink_channels[channel].active ? true : false;
}

static void Stink_CacheSounds(sfxinfo_t *sounds, int num)
{
	(void)sounds; (void)num;     /* on-demand load only */
}

sound_module_t DG_sound_module = {
	stink_sfx_devices,
	(int)(sizeof(stink_sfx_devices) / sizeof(stink_sfx_devices[0])),
	Stink_SfxInit,
	Stink_SfxShutdown,
	Stink_GetSfxLumpNum,
	Stink_SfxUpdate,
	Stink_SfxUpdateParams,
	Stink_StartSound,
	Stink_StopSound,
	Stink_SoundIsPlaying,
	Stink_CacheSounds,
};

/* ---- music module: stubs ---- */

static boolean Stink_MusicInit(void)              { return true; }
static void    Stink_MusicShutdown(void)          { }
static void    Stink_MusicSetVolume(int v)        { (void)v; }
static void    Stink_MusicPause(void)             { }
static void    Stink_MusicResume(void)            { }
static void   *Stink_MusicRegister(void *d, int l){ (void)d; (void)l; return (void *)0; }
static void    Stink_MusicUnregister(void *h)     { (void)h; }
static void    Stink_MusicPlay(void *h, boolean lp){ (void)h; (void)lp; }
static void    Stink_MusicStop(void)              { }
static boolean Stink_MusicIsPlaying(void)         { return false; }
static void    Stink_MusicPoll(void)              { }

music_module_t DG_music_module = {
	stink_music_devices,
	(int)(sizeof(stink_music_devices) / sizeof(stink_music_devices[0])),
	Stink_MusicInit,
	Stink_MusicShutdown,
	Stink_MusicSetVolume,
	Stink_MusicPause,
	Stink_MusicResume,
	Stink_MusicRegister,
	Stink_MusicUnregister,
	Stink_MusicPlay,
	Stink_MusicStop,
	Stink_MusicIsPlaying,
	Stink_MusicPoll,
};
