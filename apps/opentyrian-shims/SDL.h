#ifndef SDL_H
#define SDL_H

#include <stdint.h>
#include <stddef.h>

#define SDL_INIT_VIDEO 0x00000020
#define SDL_INIT_AUDIO 0x00000010
#define SDL_INIT_JOYSTICK 0x00000200
#define SDL_INIT_TIMER 0x00000001

#define SDL_SWSURFACE 0
#define SDL_HWSURFACE 1
#define SDL_DOUBLEBUF 1073741824
#define SDL_FULLSCREEN 2147483648

#define SDL_APPACTIVE 0x04

typedef struct {
    uint8_t r, g, b, unused;
} SDL_Color;

typedef struct {
    int ncolors;
    SDL_Color *colors;
} SDL_Palette;

typedef struct {
    SDL_Palette *palette;
    uint8_t BitsPerPixel;
    uint8_t BytesPerPixel;
    uint8_t Rloss, Gloss, Bloss, Aloss;
    uint8_t Rshift, Gshift, Bshift, Ashift;
    uint32_t Rmask, Gmask, Bmask, Amask;
    uint32_t colorkey;
    uint8_t alpha;
} SDL_PixelFormat;

typedef struct {
    int16_t x, y;
    uint16_t w, h;
} SDL_Rect;

typedef struct SDL_Surface {
    uint32_t flags;
    SDL_PixelFormat *format;
    int w, h;
    uint16_t pitch;
    void *pixels;
    int offset;
    struct private_hwdata *hwdata;
    SDL_Rect clip_rect;
    uint32_t unused1;
    uint32_t locked;
    struct SDL_BlitMap *map;
    unsigned int format_version;
    int refcount;
} SDL_Surface;

typedef struct {
    uint8_t scancode;
    int sym;
    uint16_t mod;
    uint16_t unicode;
} SDL_keysym;

typedef struct {
    uint8_t type;
    uint8_t state;
    SDL_keysym keysym;
} SDL_KeyboardEvent;

typedef struct {
    uint8_t type;
    uint8_t button;
    uint8_t state;
    uint16_t x, y;
} SDL_MouseButtonEvent;

typedef struct {
    uint8_t type;
    uint8_t state;
    uint16_t x, y;
    int16_t xrel, yrel;
} SDL_MouseMotionEvent;

typedef struct {
    uint8_t type;
} SDL_QuitEvent;

typedef struct {
    uint8_t type;
    uint8_t gain;
    uint8_t state;
} SDL_ActiveEvent;

typedef union {
    uint8_t type;
    SDL_ActiveEvent active;
    SDL_KeyboardEvent key;
    SDL_MouseMotionEvent motion;
    SDL_MouseButtonEvent button;
    SDL_QuitEvent quit;
} SDL_Event;

#define SDL_ACTIVEEVENT 1
#define SDL_KEYDOWN 2
#define SDL_KEYUP 3
#define SDL_MOUSEMOTION 4
#define SDL_MOUSEBUTTONDOWN 5
#define SDL_MOUSEBUTTONUP 6
#define SDL_QUIT 12

/* Audio */
typedef struct {
    int freq;
    uint16_t format;
    uint8_t channels;
    uint8_t silence;
    uint16_t samples;
    uint16_t padding;
    uint32_t size;
    void (*callback)(void *userdata, uint8_t *stream, int len);
    void *userdata;
} SDL_AudioSpec;

#define AUDIO_U8 0x0008
#define AUDIO_S8 0x8008
#define AUDIO_U16LSB 0x0010
#define AUDIO_S16LSB 0x8010
#define AUDIO_U16MSB 0x1010
#define AUDIO_S16MSB 0x9010
#define AUDIO_U16 AUDIO_U16LSB
#define AUDIO_S16 AUDIO_S16LSB

#define SDL_MUTEX_TIMEDOUT 1
#define SDL_MUTEX_MAXWAIT (~(uint32_t)0)

typedef struct SDL_mutex SDL_mutex;

/* Keyboard codes */
#define SDL_SCANCODE_UP 82
#define SDL_SCANCODE_DOWN 81
#define SDL_SCANCODE_LEFT 80
#define SDL_SCANCODE_RIGHT 79
#define SDL_SCANCODE_SPACE 44
#define SDL_SCANCODE_RETURN 40
#define SDL_SCANCODE_ESCAPE 41
#define SDL_SCANCODE_LALT 226
#define SDL_SCANCODE_RALT 230
#define SDL_SCANCODE_LCTRL 224
#define SDL_SCANCODE_RCTRL 228
#define SDL_SCANCODE_1 30
#define SDL_SCANCODE_2 31
#define SDL_SCANCODE_3 32

/* Mouse */
#define SDL_BUTTON_LEFT 1
#define SDL_BUTTON_MIDDLE 2
#define SDL_BUTTON_RIGHT 3
#define SDL_BUTTON_WHEELUP 4
#define SDL_BUTTON_WHEELDOWN 5

/* Functions */
int SDL_Init(uint32_t flags);
void SDL_Quit(void);
char *SDL_GetError(void);
void SDL_WM_SetCaption(const char *title, const char *icon);

SDL_Surface *SDL_SetVideoMode(int width, int height, int bpp, uint32_t flags);
void SDL_FreeSurface(SDL_Surface *surface);
int SDL_FillRect(SDL_Surface *dst, SDL_Rect *dstrect, uint32_t color);
int SDL_Flip(SDL_Surface *screen);
void SDL_UpdateRect(SDL_Surface *screen, int32_t x, int32_t y, uint32_t w, uint32_t h);
int SDL_SetColors(SDL_Surface *surface, SDL_Color *colors, int firstcolor, int ncolors);
int SDL_LockSurface(SDL_Surface *surface);
void SDL_UnlockSurface(SDL_Surface *surface);
uint8_t SDL_GetMouseState(int *x, int *y);

void SDL_ShowCursor(int toggle);

int SDL_PollEvent(SDL_Event *event);
int SDL_WaitEvent(SDL_Event *event);

uint32_t SDL_GetTicks(void);
void SDL_Delay(uint32_t ms);

int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained);
void SDL_CloseAudio(void);
void SDL_PauseAudio(int pause_on);
void SDL_LockAudio(void);
void SDL_UnlockAudio(void);

SDL_mutex *SDL_CreateMutex(void);
void SDL_DestroyMutex(SDL_mutex *mutex);
int SDL_LockMutex(SDL_mutex *mutex);
int SDL_UnlockMutex(SDL_mutex *mutex);

int SDL_GetNumVideoDisplays(void);

typedef void* SDL_Joystick;
int SDL_NumJoysticks(void);
SDL_Joystick *SDL_JoystickOpen(int device_index);
const char *SDL_JoystickName(int device_index);
void SDL_JoystickClose(SDL_Joystick *joystick);

#endif
