#ifndef SDL_H
#define SDL_H

#include <stdint.h>
#include <stddef.h>

typedef int8_t Sint8;
typedef uint8_t Uint8;
typedef int16_t Sint16;
typedef uint16_t Uint16;
typedef int32_t Sint32;
typedef uint32_t Uint32;
typedef int64_t Sint64;
typedef uint64_t Uint64;

typedef int32_t SDL_Keycode;
typedef int32_t SDL_Scancode;
typedef struct SDL_Window SDL_Window;
typedef struct SDL_Texture SDL_Texture;

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
    int x, y;
    int w, h;
} SDL_Rect;

typedef struct SDL_Renderer SDL_Renderer;

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
    uint8_t repeat;
    SDL_keysym keysym;
} SDL_KeyboardEvent;

typedef struct {
    uint8_t type;
    uint8_t button;
    uint8_t state;
    int32_t x, y;
} SDL_MouseButtonEvent;

typedef struct {
    uint8_t type;
    uint8_t state;
    int32_t x, y;
    int32_t xrel, yrel;
} SDL_MouseMotionEvent;

typedef struct {
    uint8_t type;
} SDL_QuitEvent;

typedef struct {
    uint8_t type;
    uint8_t gain;
    uint8_t state;
} SDL_ActiveEvent;

typedef struct {
    uint8_t type;
    char text[32];
} SDL_TextInputEvent;

typedef struct {
    uint8_t type;
    uint8_t event;
    uint8_t data1, data2;
} SDL_WindowEvent;

typedef union {
    uint8_t type;
    SDL_ActiveEvent active;
    SDL_KeyboardEvent key;
    SDL_MouseMotionEvent motion;
    SDL_MouseButtonEvent button;
    SDL_QuitEvent quit;
    SDL_TextInputEvent text;
    SDL_WindowEvent window;
} SDL_Event;

#define SDL_ACTIVEEVENT 1
#define SDL_KEYDOWN 2
#define SDL_KEYUP 3
#define SDL_MOUSEMOTION 4
#define SDL_MOUSEBUTTONDOWN 5
#define SDL_MOUSEBUTTONUP 6
#define SDL_WINDOWEVENT 7
#define SDL_TEXTINPUT 8
#define SDL_QUIT 12

#define SDL_WINDOWEVENT_FOCUS_LOST 1
#define SDL_WINDOWEVENT_FOCUS_GAINED 2
#define SDL_WINDOWEVENT_RESIZED 3
#define SDL_WINDOW_HIDDEN 8
#define SDL_WINDOW_RESIZABLE 32
#define SDL_WINDOW_FULLSCREEN_DESKTOP 4097
#define SDL_WINDOWPOS_CENTERED 0x2FFF0000
#define SDL_PIXELFORMAT_RGB888 1
#define SDL_PIXELFORMAT_RGB565 2
#define SDL_TEXTUREACCESS_STREAMING 1
#define SDL_FALSE 0
#define SDL_TRUE 1
#define SDL_VERSION_ATLEAST(X,Y,Z) 1

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
#define AUDIO_S16SYS AUDIO_S16
#define SDL_AUDIO_ALLOW_FREQUENCY_CHANGE 1
#define SDL_AUDIO_ALLOW_SAMPLES_CHANGE 2

typedef uint32_t SDL_AudioDeviceID;

typedef struct SDL_AudioCVT {
    int needed;
    uint16_t src_format;
    uint16_t dst_format;
    double rate_incr;
    uint8_t *buf;
    int len;
    int len_cvt;
    int len_mult;
    double len_ratio;
    void (*filters[10])(struct SDL_AudioCVT *cvt, uint16_t format);
    int filter_index;
} SDL_AudioCVT;

int SDL_BuildAudioCVT(SDL_AudioCVT *cvt, uint16_t src_format, uint8_t src_channels, int src_rate, uint16_t dst_format, uint8_t dst_channels, int dst_rate);
int SDL_ConvertAudio(SDL_AudioCVT *cvt);

SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int iscapture, const SDL_AudioSpec *desired, SDL_AudioSpec *obtained, int allowed_changes);
void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on);
void SDL_CloseAudioDevice(SDL_AudioDeviceID dev);
void SDL_LockAudioDevice(SDL_AudioDeviceID dev);
void SDL_UnlockAudioDevice(SDL_AudioDeviceID dev);

#define SDL_MUTEX_TIMEDOUT 1
#define SDL_MUTEX_MAXWAIT (~(uint32_t)0)

typedef struct SDL_mutex SDL_mutex;

/* Keyboard codes */
#define SDL_SCANCODE_UNKNOWN 0
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
#define SDL_SCANCODE_4 33
#define SDL_SCANCODE_5 34
#define SDL_SCANCODE_6 35
#define SDL_SCANCODE_7 36
#define SDL_SCANCODE_8 37
#define SDL_SCANCODE_9 38
#define SDL_SCANCODE_0 39
#define SDL_SCANCODE_CAPSLOCK 57
#define SDL_SCANCODE_F2 59
#define SDL_SCANCODE_F3 60
#define SDL_SCANCODE_F4 61
#define SDL_SCANCODE_F5 62
#define SDL_SCANCODE_F6 63
#define SDL_SCANCODE_F7 64
#define SDL_SCANCODE_F8 65
#define SDL_SCANCODE_F9 66
#define SDL_SCANCODE_F12 70
#define SDL_SCANCODE_NUMLOCKCLEAR 83
#define SDL_SCANCODE_SCROLLLOCK 71
#define SDL_SCANCODE_MINUS 45
#define SDL_SCANCODE_EQUALS 46
#define SDL_SCANCODE_LEFTBRACKET 47
#define SDL_SCANCODE_RIGHTBRACKET 48
#define SDL_SCANCODE_GRAVE 53
#define SDL_SCANCODE_DELETE 76
#define SDL_SCANCODE_F10 68
#define SDL_SCANCODE_F11 69
#define SDL_SCANCODE_P 19
#define SDL_SCANCODE_F1 58
#define SDL_SCANCODE_BACKSPACE 42
#define SDL_SCANCODE_KP_6 90
#define SDL_SCANCODE_KP_8 91
#define SDL_SCANCODE_KP_2 92
#define SDL_SCANCODE_BACKSLASH 43
#define SDL_SCANCODE_KP_5 93
#define SDL_SCANCODE_INSERT 73
#define SDL_SCANCODE_KP_0 98
#define SDL_SCANCODE_KP_ENTER 96
#define SDL_SCANCODE_PAGEUP 75
#define SDL_SCANCODE_KP_9 94
#define SDL_SCANCODE_PAGEDOWN 78
#define SDL_SCANCODE_KP_3 95
#define SDL_SCANCODE_A 4
#define SDL_SCANCODE_B 5
#define SDL_SCANCODE_C 6
#define SDL_SCANCODE_D 7
#define SDL_SCANCODE_E 8
#define SDL_SCANCODE_F 9
#define SDL_SCANCODE_G 10
#define SDL_SCANCODE_H 11
#define SDL_SCANCODE_I 12
#define SDL_SCANCODE_J 13
#define SDL_SCANCODE_K 14
#define SDL_SCANCODE_L 15
#define SDL_SCANCODE_M 16
#define SDL_SCANCODE_N 17
#define SDL_SCANCODE_O 18
#define SDL_SCANCODE_Q 20
#define SDL_SCANCODE_R 21
#define SDL_SCANCODE_S 22
#define SDL_SCANCODE_T 23
#define SDL_SCANCODE_U 24
#define SDL_SCANCODE_V 25
#define SDL_SCANCODE_W 26
#define SDL_SCANCODE_X 27
#define SDL_SCANCODE_Y 28
#define SDL_SCANCODE_Z 29
#define SDL_SCANCODE_LSHIFT 225
#define SDL_SCANCODE_KP_4 89
#define SDL_SCANCODE_KP_7 99
#define SDL_SCANCODE_KP_1 88
#define SDL_SCANCODE_SLASH 56
#define SDL_SCANCODE_HOME 74
#define SDL_SCANCODE_END 77
#define SDL_SCANCODE_TAB 43
#define KMOD_NONE 0x0000
#define SDLK_s 115
#define SDLK_l 108
#define SDLK_o 111
#define SDLK_r 114
#define SDLK_d 100
#define SDLK_g 103
#define SDLK_RIGHTBRACKET 93
#define KMOD_ALT 0x0100
#define KMOD_SHIFT 0x0001
#define KMOD_CTRL 0x0040
#define KMOD_GUI 0x0400
#define SDL_SCANCODE_COMMA 54
#define SDL_SCANCODE_PERIOD 55
#define SDL_SCANCODE_SEMICOLON 51
#define SDL_NUM_SCANCODES 256
#define SDL_ENABLE 1
#define SDL_DISABLE 0

static inline uint16_t SDL_Swap16(uint16_t x) {
    return (x << 8) | (x >> 8);
}
static inline uint32_t SDL_Swap32(uint32_t x) {
    return (x << 24) | ((x << 8) & 0x00FF0000) | ((x >> 8) & 0x0000FF00) | (x >> 24);
}

#define SDL_SwapLE16(X) (X)
#define SDL_SwapLE32(X) (X)

/* Mouse and Joystick */
#define SDL_BUTTON_LEFT 1
#define SDL_BUTTON_MIDDLE 2
#define SDL_BUTTON_RIGHT 3
#define SDL_BUTTON_WHEELUP 4
#define SDL_BUTTON_WHEELDOWN 5
#define SDL_BUTTON(X) (1 << ((X)-1))
#define SDL_BUTTON_LMASK SDL_BUTTON(SDL_BUTTON_LEFT)
#define SDL_BUTTON_MMASK SDL_BUTTON(SDL_BUTTON_MIDDLE)
#define SDL_BUTTON_RMASK SDL_BUTTON(SDL_BUTTON_RIGHT)
#define SDL_HAT_CENTERED 0
#define SDL_HAT_UP 1
#define SDL_HAT_RIGHT 2
#define SDL_HAT_DOWN 4
#define SDL_HAT_LEFT 8
#define SDL_RELEASED 0
#define SDL_PRESSED 1
#define SDL_IGNORE 0

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

#define SDL_MUSTLOCK(S) (((S)->flags & 1) != 0)

void SDL_ShowCursor(int toggle);
int SDL_BlitSurface(SDL_Surface *src, SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect);
uint32_t SDL_MapRGB(const SDL_PixelFormat *format, uint8_t r, uint8_t g, uint8_t b);
size_t SDL_strlcpy(char *dst, const char *src, size_t maxlen);

int SDL_WasInit(uint32_t flags);
SDL_Surface *SDL_CreateRGBSurface(uint32_t flags, int width, int height, int depth, uint32_t Rmask, uint32_t Gmask, uint32_t Bmask, uint32_t Amask);
void *SDL_CreateWindow(const char *title, int x, int y, int w, int h, uint32_t flags);

void SDL_ShowWindow(void *window);
int SDL_SetRenderDrawColor(void *renderer, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
int SDL_RenderClear(void *renderer);
void SDL_RenderPresent(void *renderer);
void SDL_DestroyWindow(void *window);
void *SDL_CreateRenderer(void *window, int index, uint32_t flags);
void SDL_DestroyRenderer(void *renderer);
SDL_PixelFormat *SDL_AllocFormat(uint32_t pixel_format);
void *SDL_CreateTexture(void *renderer, uint32_t format, int access, int w, int h);
const char *SDL_GetPixelFormatName(uint32_t format);
void SDL_DestroyTexture(void *texture);
void SDL_FreeFormat(SDL_PixelFormat *format);
int SDL_GetWindowDisplayIndex(void *window);
void SDL_GetWindowSize(void *window, int *w, int *h);
int SDL_GetDisplayBounds(int displayIndex, SDL_Rect *rect);
void SDL_SetWindowPosition(void *window, int x, int y);
int SDL_SetWindowFullscreen(void *window, uint32_t flags);
void SDL_SetWindowSize(void *window, int w, int h);
int SDL_QueryTexture(void *texture, uint32_t *format, int *access, int *w, int *h);
int SDL_RenderCopy(void *renderer, void *texture, const SDL_Rect *srcrect, const SDL_Rect *dstrect);
int SDL_LockTexture(void *texture, const SDL_Rect *rect, void **pixels, int *pitch);
void SDL_UnlockTexture(void *texture);

int SDL_PollEvent(SDL_Event *event);
int SDL_WaitEvent(SDL_Event *event);
void SDL_StopTextInput(void);
void SDL_StartTextInput(void);
int SDL_SetRelativeMouseMode(int enabled);
#define SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE "SDL_MOUSE_RELATIVE_SYSTEM_SCALE"
int SDL_SetHint(const char *name, const char *value);

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
const char *SDL_JoystickName(SDL_Joystick *joystick);
int SDL_JoystickNumAxes(SDL_Joystick *joystick);
int SDL_JoystickNumButtons(SDL_Joystick *joystick);
int SDL_JoystickNumHats(SDL_Joystick *joystick);
void SDL_JoystickClose(SDL_Joystick *joystick);
void SDL_JoystickUpdate(void);
int SDL_JoystickEventState(int state);
int SDL_JoystickGetAxis(SDL_Joystick *joystick, int axis);
uint8_t SDL_JoystickGetButton(SDL_Joystick *joystick, int button);
uint8_t SDL_JoystickGetHat(SDL_Joystick *joystick, int hat);

int SDL_PushEvent(SDL_Event *event);
int SDL_InitSubSystem(uint32_t flags);
void SDL_QuitSubSystem(uint32_t flags);

int32_t SDL_GetScancodeFromName(const char *name);
const char *SDL_GetScancodeName(int32_t scancode);

#endif
