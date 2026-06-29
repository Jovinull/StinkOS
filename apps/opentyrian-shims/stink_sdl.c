#include "SDL.h"
#include <libstink.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static SDL_Surface *screen = NULL;

int SDL_Init(uint32_t flags) {
    (void)flags;
    sys_log("SDL_Init called");
    return 0;
}

void SDL_Quit(void) {
    sys_log("SDL_Quit called");
}

char *SDL_GetError(void) {
    return "No error";
}

void SDL_WM_SetCaption(const char *title, const char *icon) {
    (void)title;
    (void)icon;
    /* StinkOS handles window titles natively if windowed, or it's fullscreen */
}

SDL_Surface *SDL_SetVideoMode(int width, int height, int bpp, uint32_t flags) {
    (void)flags;
    sys_log("SDL_SetVideoMode");
    
    if (screen) {
        if (screen->pixels) free(screen->pixels);
        free(screen->format);
        free(screen);
    }
    
    screen = malloc(sizeof(SDL_Surface));
    memset(screen, 0, sizeof(SDL_Surface));
    screen->w = width;
    screen->h = height;
    screen->format = malloc(sizeof(SDL_PixelFormat));
    memset(screen->format, 0, sizeof(SDL_PixelFormat));
    screen->format->BitsPerPixel = bpp;
    screen->format->BytesPerPixel = bpp / 8;
    screen->pitch = width * (bpp / 8);
    screen->pixels = malloc(height * screen->pitch);
    
    /* TODO: Set actual video mode if StinkOS supports it */
    
    return screen;
}

void SDL_FreeSurface(SDL_Surface *surface) {
    if (!surface) return;
    if (surface->pixels) free(surface->pixels);
    if (surface->format) free(surface->format);
    free(surface);
}

int SDL_FillRect(SDL_Surface *dst, SDL_Rect *dstrect, uint32_t color) {
    if (!dst) return -1;
    /* Minimally stubbed */
    return 0;
}

int SDL_Flip(SDL_Surface *screen_surf) {
    /* For StinkOS, we need to push the pixels from screen_surf->pixels to the real framebuffer */
    /* If bpp=8 (VGA palette), we need to translate palette to 32-bit ARGB */
    return 0;
}

void SDL_UpdateRect(SDL_Surface *screen_surf, int32_t x, int32_t y, uint32_t w, uint32_t h) {
    SDL_Flip(screen_surf);
}

int SDL_SetColors(SDL_Surface *surface, SDL_Color *colors, int firstcolor, int ncolors) {
    return 1;
}

int SDL_LockSurface(SDL_Surface *surface) {
    return 0;
}

void SDL_UnlockSurface(SDL_Surface *surface) {
}

uint8_t SDL_GetMouseState(int *x, int *y) {
    if (x) *x = 0;
    if (y) *y = 0;
    return 0;
}

void SDL_ShowCursor(int toggle) {
    (void)toggle;
}

int SDL_PollEvent(SDL_Event *event) {
    unsigned int ev = sys_get_keyevent();
    if (ev) {
        int sc = ev & KEY_EV_SC_MASK;
        int pressed = (ev & KEY_EV_PRESSED) ? 1 : 0;
        
        event->type = pressed ? SDL_KEYDOWN : SDL_KEYUP;
        event->key.state = pressed;
        event->key.keysym.scancode = sc;
        
        /* Map common keys */
        if (sc == KEY_UP) event->key.keysym.scancode = SDL_SCANCODE_UP;
        if (sc == KEY_DOWN) event->key.keysym.scancode = SDL_SCANCODE_DOWN;
        if (sc == KEY_LEFT) event->key.keysym.scancode = SDL_SCANCODE_LEFT;
        if (sc == KEY_RIGHT) event->key.keysym.scancode = SDL_SCANCODE_RIGHT;
        if (sc == ' ') event->key.keysym.scancode = SDL_SCANCODE_SPACE;
        if (sc == 27) event->key.keysym.scancode = SDL_SCANCODE_ESCAPE;
        if (sc == 13) event->key.keysym.scancode = SDL_SCANCODE_RETURN;
        
        return 1;
    }
    return 0;
}

int SDL_WaitEvent(SDL_Event *event) {
    while (!SDL_PollEvent(event)) {
        sys_sleep_ms(10);
    }
    return 1;
}

uint32_t SDL_GetTicks(void) {
    return sys_ticks() * 10; /* Assuming 100Hz tick rate in StinkOS, converts to ms */
}

void SDL_Delay(uint32_t ms) {
    sys_sleep_ms(ms);
}

int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained) {
    if (obtained) {
        memcpy(obtained, desired, sizeof(SDL_AudioSpec));
    }
    return 0;
}

void SDL_CloseAudio(void) {}
void SDL_PauseAudio(int pause_on) {}
void SDL_LockAudio(void) {}
void SDL_UnlockAudio(void) {}

SDL_mutex *SDL_CreateMutex(void) { return (SDL_mutex *)1; }
void SDL_DestroyMutex(SDL_mutex *mutex) { (void)mutex; }
int SDL_LockMutex(SDL_mutex *mutex) { return 0; }
int SDL_UnlockMutex(SDL_mutex *mutex) { return 0; }

int SDL_GetNumVideoDisplays(void) { return 1; }

int SDL_NumJoysticks(void) { return 0; }
SDL_Joystick *SDL_JoystickOpen(int device_index) { return NULL; }
const char *SDL_JoystickName(SDL_Joystick *joystick) { return "None"; }
int SDL_JoystickNumAxes(SDL_Joystick *joystick) { return 0; }
int SDL_JoystickNumButtons(SDL_Joystick *joystick) { return 0; }
int SDL_JoystickNumHats(SDL_Joystick *joystick) { return 0; }
void SDL_JoystickClose(SDL_Joystick *joystick) {}
void SDL_JoystickUpdate(void) {}
int SDL_JoystickEventState(int state) { return 0; }
int SDL_JoystickGetAxis(SDL_Joystick *joystick, int axis) { return 0; }
uint8_t SDL_JoystickGetButton(SDL_Joystick *joystick, int button) { return 0; }
uint8_t SDL_JoystickGetHat(SDL_Joystick *joystick, int hat) { return SDL_HAT_CENTERED; }

int SDL_PushEvent(SDL_Event *event) { return 0; }
int SDL_InitSubSystem(uint32_t flags) { return 0; }
void SDL_QuitSubSystem(uint32_t flags) {}

int32_t SDL_GetScancodeFromName(const char *name) { return 0; }
const char *SDL_GetScancodeName(int32_t scancode) { return "Unknown"; }

float roundf(float x) { return x; }
float sinf(float x) { return 0.0f; }
float cosf(float x) { return 1.0f; }
float floorf(float x) { return x; }
double sin(double x) { return 0.0; }
double cos(double x) { return 1.0; }
double floor(double x) { return x; }
double round(double x) { return x; }

float fabsf(float x) { return x < 0 ? -x : x; }
float sqrtf(float x) { return x; }
float atanf(float x) { return x; }
float powf(float x, float y) { return x; }
double pow(double x, double y) { return x; }

int SDL_BlitSurface(SDL_Surface *src, SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect) { return 0; }
uint32_t SDL_MapRGB(const SDL_PixelFormat *format, uint8_t r, uint8_t g, uint8_t b) { return 0; }

void SDL_ShowWindow(void *window) {}
int SDL_SetRenderDrawColor(void *renderer, uint8_t r, uint8_t g, uint8_t b, uint8_t a) { return 0; }
int SDL_RenderClear(void *renderer) { return 0; }
void SDL_RenderPresent(void *renderer) {}
void SDL_DestroyWindow(void *window) {}
void *SDL_CreateRenderer(void *window, int index, uint32_t flags) { return NULL; }
void SDL_DestroyRenderer(void *renderer) {}
SDL_PixelFormat *SDL_AllocFormat(uint32_t pixel_format) { return NULL; }
void *SDL_CreateTexture(void *renderer, uint32_t format, int access, int w, int h) { return NULL; }
const char *SDL_GetPixelFormatName(uint32_t format) { return ""; }
void SDL_DestroyTexture(void *texture) {}
void SDL_FreeFormat(SDL_PixelFormat *format) {}
int SDL_GetWindowDisplayIndex(void *window) { return 0; }
void SDL_GetWindowSize(void *window, int *w, int *h) {}
int SDL_GetDisplayBounds(int displayIndex, SDL_Rect *rect) { return 0; }
void SDL_SetWindowPosition(void *window, int x, int y) {}
int SDL_SetWindowFullscreen(void *window, uint32_t flags) { return 0; }
void SDL_SetWindowSize(void *window, int w, int h) {}
int SDL_QueryTexture(void *texture, uint32_t *format, int *access, int *w, int *h) { return 0; }
int SDL_RenderCopy(void *renderer, void *texture, const SDL_Rect *srcrect, const SDL_Rect *dstrect) { return 0; }
int SDL_LockTexture(void *texture, const SDL_Rect *rect, void **pixels, int *pitch) { return 0; }
void SDL_UnlockTexture(void *texture) {}

#include <time.h>
struct tm *localtime(const time_t *timep) {
    static struct tm t;
    return &t;
}

int SDL_WasInit(uint32_t flags) { return 0; }
SDL_Surface *SDL_CreateRGBSurface(uint32_t flags, int width, int height, int depth, uint32_t Rmask, uint32_t Gmask, uint32_t Bmask, uint32_t Amask) { return NULL; }
void *SDL_CreateWindow(const char *title, int x, int y, int w, int h, uint32_t flags) { return NULL; }

SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int iscapture, const SDL_AudioSpec *desired, SDL_AudioSpec *obtained, int allowed_changes) { return 1; }
void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on) {}
void SDL_CloseAudioDevice(SDL_AudioDeviceID dev) {}
void SDL_LockAudioDevice(SDL_AudioDeviceID dev) {}
void SDL_UnlockAudioDevice(SDL_AudioDeviceID dev) {}

int SDL_BuildAudioCVT(SDL_AudioCVT *cvt, uint16_t src_format, uint8_t src_channels, int src_rate, uint16_t dst_format, uint8_t dst_channels, int dst_rate) { return 0; }
int SDL_ConvertAudio(SDL_AudioCVT *cvt) { return 0; }

void SDL_StopTextInput(void) {}
void SDL_StartTextInput(void) {}
int SDL_SetRelativeMouseMode(int enabled) { return 0; }
int SDL_SetHint(const char *name, const char *value) { return 0; }

size_t SDL_strlcpy(char *dst, const char *src, size_t maxlen) {
    size_t srclen = strlen(src);
    if (maxlen > 0) {
        size_t len = (srclen < maxlen - 1) ? srclen : (maxlen - 1);
        memcpy(dst, src, len);
        dst[len] = '\0';
    }
    return srclen;
}
