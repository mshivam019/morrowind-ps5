/* PS5 OpenGL - SDL2 video driver for the frozen G19 public EGL contract.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Built inside SDL2; public SDL APIs and the upstream event queue are unchanged.
 */
#include "SDL_internal.h"
#include "SDL_timer.h"
#include "video/SDL_sysvideo.h"
#include "events/SDL_keyboard_c.h"
#if defined(__PROSPERO__)
#include "video/ps5/SDL_ps5keyboard.h"
#endif
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "ps5g19_display.h"

/* ponytail: one fixed-profile window/context on the video thread; extend only with a
 * separately validated EGL ownership contract. No private presentation API. */
typedef struct {
    EGLDisplay display;
    EGLConfig config;
    EGLSurface surface;
    EGLContext context;
    SDL_Window *window;
    SDL_threadID thread;
    int interval;
} G19;

/* Rate the runtime actually drives the display at; high refresh is only used
 * when the display accepted it, so start from 60 until presentation says so. */
/* The packaged SDK uses the fixed 1920x1080@60 display profile. */

static void sync_refresh_rate(SDL_Window *window)
{
    SDL_VideoDisplay *display = SDL_GetDisplayForWindow(window);
    const int hz = 60;
    if (!display || display->current_mode.refresh_rate == hz) return;
    display->current_mode.refresh_rate = display->desktop_mode.refresh_rate = hz;
    for (int i = 0; i < display->num_display_modes; ++i)
        display->display_modes[i].refresh_rate = hz;
    window->fullscreen_mode.refresh_rate = hz;
}

static int egl_error(const char *operation)
{
    return SDL_SetError("G19 %s: EGL error 0x%04x", operation, eglGetError());
}

static int on_thread(G19 *g)
{
    return SDL_ThreadID() == g->thread ? 0 :
        SDL_SetError("G19 requires the video thread");
}

static int video_init(_THIS)
{
    G19 *g = _this->driverdata;
    int index;
    SDL_DisplayMode mode = { SDL_PIXELFORMAT_ABGR8888, PS5_OPENGL_NATIVE_WIDTH,
                            PS5_OPENGL_NATIVE_HEIGHT, 60, NULL };
    g->thread = SDL_ThreadID();
    g->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g->display == EGL_NO_DISPLAY) return egl_error("eglGetDisplay");
    if (!eglInitialize(g->display, NULL, NULL)) {
        g->display = EGL_NO_DISPLAY;
        return egl_error("eglInitialize");
    }
    index = SDL_AddBasicVideoDisplay(&mode);
    if (index < 0) return -1;
    return SDL_AddDisplayMode(&_this->displays[index], &mode) ? 0 : -1;
}

static int load_library(_THIS, const char *path)
{
    (void)_this;
    return path ? SDL_SetError("G19 is statically linked; use a NULL GL library path") : 0;
}

static void *get_proc(_THIS, const char *name)
{
    void *proc;
    (void)_this;
    if (!name || !*name) {
        SDL_SetError("G19 requires a GL procedure name");
        return NULL;
    }
    proc = (void *)eglGetProcAddress(name);
    if (!proc) SDL_SetError("G19 has no GL procedure %s", name);
    return proc;
}

static int create_window(_THIS, SDL_Window *window)
{
    G19 *g = _this->driverdata;
    EGLint count = 0, width = 0, height = 0;
    const EGLint attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, _this->gl_config.red_size,
        EGL_GREEN_SIZE, _this->gl_config.green_size,
        EGL_BLUE_SIZE, _this->gl_config.blue_size,
        EGL_ALPHA_SIZE, _this->gl_config.alpha_size,
        EGL_DEPTH_SIZE, _this->gl_config.depth_size,
        EGL_STENCIL_SIZE, _this->gl_config.stencil_size, EGL_NONE
    };
    if (on_thread(g) < 0) return -1;
    if (g->window || g->surface || window->w != PS5_OPENGL_NATIVE_WIDTH ||
        window->h != PS5_OPENGL_NATIVE_HEIGHT ||
        !(window->flags & SDL_WINDOW_OPENGL) || (window->flags & SDL_WINDOW_RESIZABLE))
        return SDL_SetError("G19 requires one fixed %dx%d OpenGL window",
                            PS5_OPENGL_NATIVE_WIDTH, PS5_OPENGL_NATIVE_HEIGHT);
    if (_this->gl_config.stereo || _this->gl_config.multisamplebuffers ||
        _this->gl_config.multisamplesamples || _this->gl_config.floatbuffers ||
        _this->gl_config.framebuffer_srgb_capable || _this->gl_config.accum_red_size ||
        _this->gl_config.accum_green_size || _this->gl_config.accum_blue_size ||
        _this->gl_config.accum_alpha_size || _this->gl_config.buffer_size > 32 ||
        !_this->gl_config.double_buffer)
        return SDL_SetError("G19 SDL supports double-buffered RGBA8 without MSAA, stereo, accumulation or sRGB");
    if (!eglChooseConfig(g->display, attributes, &g->config, 1, &count))
        return egl_error("eglChooseConfig");
    if (count != 1) return SDL_SetError("G19 has no matching EGL config");
    g->surface = eglCreateWindowSurface(g->display, g->config, (EGLNativeWindowType)0, NULL);
    if (!g->surface) return egl_error("eglCreateWindowSurface");
    /* Establish ownership before queries, so SDL's failed-create cleanup works. */
    g->window = window;
    if (!eglQuerySurface(g->display, g->surface, EGL_WIDTH, &width) ||
        !eglQuerySurface(g->display, g->surface, EGL_HEIGHT, &height))
        return egl_error("eglQuerySurface");
    if (width != window->w || height != window->h)
        return SDL_SetError("G19 EGL drawable %dx%d does not match the requested %dx%d window",
                            width, height, window->w, window->h);
    g->interval = 1;
#if defined(__PROSPERO__)
    if (PS5_Keyboard_Init() < 0 || PS5_Keyboard_Open() < 0)
        SDL_Log("PS5 keyboard setup: %s", SDL_GetError());
#endif
    SDL_SetKeyboardFocus(window); /* SDL also uses focus to gate joystick events. */
    sync_refresh_rate(window);
    return 0;
}

static int make_current(_THIS, SDL_Window *window, SDL_GLContext context)
{
    G19 *g = _this->driverdata;
    EGLSurface surface = context ? g->surface : EGL_NO_SURFACE;
    if (on_thread(g) < 0) return -1;
    if (context && (context != g->context || window != g->window))
        return SDL_SetError("G19 context/window mismatch");
    if (!eglMakeCurrent(g->display, surface, surface, (EGLContext)context))
        return egl_error("eglMakeCurrent");
    return 0;
}

static void delete_context(_THIS, SDL_GLContext context)
{
    G19 *g = _this->driverdata;
    if (on_thread(g) < 0 || !context) return;
    if (context != g->context) {
        SDL_SetError("G19 unknown context");
        return;
    }
    /* SDL_GL_DeleteContext detaches first and updates SDL's TLS. If that
     * failed, EGL rejects destruction; keep both ownership records intact. */
    if (!eglDestroyContext(g->display, g->context)) {
        egl_error("eglDestroyContext");
        return;
    }
    g->context = EGL_NO_CONTEXT;
}

static SDL_GLContext create_context(_THIS, SDL_Window *window)
{
    G19 *g = _this->driverdata;
    const EGLint attributes[] = {
        EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR,
        EGL_NONE
    };
    if (on_thread(g) < 0) return NULL;
    if (g->context || window != g->window ||
        _this->gl_config.major_version != 3 || _this->gl_config.minor_version != 3 ||
        _this->gl_config.profile_mask != SDL_GL_CONTEXT_PROFILE_COMPATIBILITY ||
        _this->gl_config.flags || _this->gl_config.share_with_current_context ||
        _this->gl_config.no_error || _this->gl_config.reset_notification ||
        _this->gl_config.release_behavior != SDL_GL_CONTEXT_RELEASE_BEHAVIOR_FLUSH) {
        SDL_SetError("G19 SDL requires one unshared OpenGL 3.3 Compatibility context with default flags");
        return NULL;
    }
    if (!eglBindAPI(EGL_OPENGL_API)) {
        egl_error("eglBindAPI");
        return NULL;
    }
    g->context = eglCreateContext(g->display, g->config, EGL_NO_CONTEXT, attributes);
    if (!g->context) {
        egl_error("eglCreateContext");
        return NULL;
    }
    if (make_current(_this, window, g->context) < 0) {
        delete_context(_this, g->context);
        return NULL;
    }
    return (SDL_GLContext)g->context;
}

static int set_interval(_THIS, int interval)
{
    G19 *g = _this->driverdata;
    if (on_thread(g) < 0) return -1;
    if (interval != 0 && interval != 1) return SDL_SetError("G19 swap interval must be 0 or 1");
    if (!eglSwapInterval(g->display, interval)) return egl_error("eglSwapInterval");
    g->interval = interval;
    return 0;
}

static int get_interval(_THIS) { return ((G19 *)_this->driverdata)->interval; }

static int swap_window(_THIS, SDL_Window *window)
{
    G19 *g = _this->driverdata;
    if (on_thread(g) < 0) return -1;
    if (window != g->window) return SDL_SetError("G19 unknown window");
#ifdef __PROSPERO__
    const Uint64 ps5SwapStart = SDL_GetPerformanceCounter();
#endif
    if (!eglSwapBuffers(g->display, g->surface)) return egl_error("eglSwapBuffers");
#ifdef __PROSPERO__
    const Uint64 ps5SwapEnd = SDL_GetPerformanceCounter();
#endif
    sync_refresh_rate(window); /* The display is opened on first presentation. */
#ifdef __PROSPERO__
    static unsigned ps5SwapFrames;
    ++ps5SwapFrames;
    if (ps5SwapFrames <= 3 || ps5SwapFrames % 60 == 0)
        SDL_Log("[ps5-sdl-swap] frame=%u egl_ms=%.3f refresh_ms=%.3f", ps5SwapFrames,
                1000.0 * (ps5SwapEnd - ps5SwapStart) / SDL_GetPerformanceFrequency(),
                1000.0 * (SDL_GetPerformanceCounter() - ps5SwapEnd) / SDL_GetPerformanceFrequency());
#endif
    return 0;
}

static void destroy_window(_THIS, SDL_Window *window)
{
    G19 *g = _this->driverdata;
    if (g->window != window) return; /* Includes rejected second-window creation. */
    if (on_thread(g) < 0) return;
    g->window = NULL;
    /* SDL contexts outlive windows; release the drawable, keep the context. */
    if (make_current(_this, NULL, NULL) < 0) return;
    if (!eglDestroySurface(g->display, g->surface)) {
        egl_error("eglDestroySurface");
        return;
    }
    g->surface = EGL_NO_SURFACE;
}

static void set_window_size(_THIS, SDL_Window *window)
{
    (void)_this;
    window->w = window->windowed.w = PS5_OPENGL_NATIVE_WIDTH;
    window->h = window->windowed.h = PS5_OPENGL_NATIVE_HEIGHT;
    SDL_SetError("G19 SDL does not support resizing");
}

static void video_quit(_THIS)
{
#if defined(__PROSPERO__)
    PS5_HideScreenKeyboard(_this, NULL);
    PS5_Keyboard_Close();
#endif
    G19 *g = _this->driverdata;
    if (on_thread(g) < 0 || !g->display) return;
    delete_context(_this, g->context);
    if (g->context) return; /* Never release resources still owned by EGL. */
    if (g->surface) {
        if (!eglDestroySurface(g->display, g->surface)) {
            egl_error("eglDestroySurface(quit)");
            return;
        }
        g->surface = EGL_NO_SURFACE;
    }
    if (!eglTerminate(g->display)) {
        egl_error("eglTerminate");
        return;
    }
    g->display = EGL_NO_DISPLAY;
}

static void free_device(_THIS)
{
    SDL_free(_this->driverdata);
    SDL_free(_this);
}

/* Required SDL callback; joystick polling follows this in SDL_PumpEvents. */
static void pump_events(_THIS) {
    (void)_this;
#if defined(__PROSPERO__)
    PS5_Keyboard_PumpEvents();
#endif
}

static SDL_VideoDevice *create_device(void)
{
    SDL_VideoDevice *device = SDL_calloc(1, sizeof(*device));
    if (!device) { SDL_OutOfMemory(); return NULL; }
    device->driverdata = SDL_calloc(1, sizeof(G19));
    if (!device->driverdata) { SDL_free(device); SDL_OutOfMemory(); return NULL; }
    device->VideoInit = video_init;
    device->VideoQuit = video_quit;
    device->PumpEvents = pump_events;
#if defined(__PROSPERO__)
    device->HasScreenKeyboardSupport = PS5_HasScreenKeyboardSupport;
    device->ShowScreenKeyboard = PS5_ShowScreenKeyboard;
    device->HideScreenKeyboard = PS5_HideScreenKeyboard;
    device->IsScreenKeyboardShown = PS5_IsScreenKeyboardShown;
#endif
    device->CreateSDLWindow = create_window;
    device->DestroyWindow = destroy_window;
    device->SetWindowSize = set_window_size;
    device->GL_LoadLibrary = load_library;
    device->GL_GetProcAddress = get_proc;
    device->GL_CreateContext = create_context;
    device->GL_MakeCurrent = make_current;
    device->GL_DeleteContext = delete_context;
    device->GL_SetSwapInterval = set_interval;
    device->GL_GetSwapInterval = get_interval;
    device->GL_SwapWindow = swap_window;
    device->free = free_device;
    /* SDL_PumpEvents already updates the upstream joystick backend. No extra
     * polling thread or replacement event queue is required by this consumer. */
    return device;
}

VideoBootStrap PS5_bootstrap = { "ps5-g19", "PS5 frozen G19 EGL", create_device, NULL };
