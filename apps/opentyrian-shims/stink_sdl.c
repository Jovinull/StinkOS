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
const char *SDL_JoystickName(int device_index) { return "None"; }
void SDL_JoystickClose(SDL_Joystick *joystick) {}
