/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2018 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include "SDL_internal.h"
#ifdef SDL_VIDEO_DRIVER_PS5
#include "SDL_error.h"
#include "SDL_events.h"
#include "SDL_stdinc.h"
#include "SDL_log.h"
#include "events/SDL_keyboard_c.h"
#include "video/ps5/SDL_ps5keyboard.h"

typedef enum SceImeDialogStatus
{
    SCE_IME_DIALOG_STATUS_NONE,
    SCE_IME_DIALOG_STATUS_RUNNING,
    SCE_IME_DIALOG_STATUS_FINISHED
} SceImeDialogStatus;

typedef int (*SceImeTextFilter)(wchar_t*, uint32_t*, const wchar_t*, uint32_t);

typedef struct SceImeDialogParam
{
    int userId;
    enum {
    SCE_IME_TYPE_DEFAULT,
    SCE_IME_TYPE_BASIC_LATIN,
    SCE_IME_TYPE_URL,
    SCE_IME_TYPE_MAIL,
    SCE_IME_TYPE_NUMBER
    } type;
    uint64_t supportedLanguages;
    enum {
    SCE_IME_ENTER_LABEL_DEFAULT,
    SCE_IME_ENTER_LABEL_SEND,
    SCE_IME_ENTER_LABEL_SEARCH,
    SCE_IME_ENTER_LABEL_GO,
    } enterLabel;
    enum {
    SCE_IME_INPUT_METHOD_DEFAULT
    } inputMethod;
    SceImeTextFilter filter;
    uint32_t option;
    uint32_t maxTextLength;
    wchar_t *inputTextBuffer;
    float posx;
    float posy;
    enum {
    SCE_IME_HALIGN_LEFT,
    SCE_IME_HALIGN_CENTER,
    SCE_IME_HALIGN_RIGHT
    } halign;
    enum {
    SCE_IME_VALIGN_TOP,
    SCE_IME_VALIGN_CENTER,
    SCE_IME_VALIGN_BOTTOM
    } valign;
    const wchar_t *placeholder;
    const wchar_t *title;
    int8_t reserved[16];
} SceImeDialogParam;

typedef struct SceImeDialogResult
{
    enum {
    SCE_IME_DIALOG_END_STATUS_OK,
    SCE_IME_DIALOG_END_STATUS_USER_CANCELED,
    SCE_IME_DIALOG_END_STATUS_ABORTED,
    } outcome;
    int8_t reserved[12];
} SceImeDialogResult;



/* Native-title adaptation of SDL's PS5 IME backend. The optional physical
 * keyboard service is not available to this app. Keep it out of startup. */
int sceUserServiceGetForegroundUser(int *);
int sceSysmoduleLoadModule(uint16_t);
int sceImeDialogInit(const SceImeDialogParam *, void *);
int sceImeDialogGetResult(SceImeDialogResult *);
int sceImeDialogTerm(void);
int sceImeDialogAbort(void);
SceImeDialogStatus sceImeDialogGetStatus(void);

/* Native optional imports may be null. Volatile preserves the runtime checks
 * instead of letting the compiler assume a declared function always exists. */
static struct {
    int (*init)(const SceImeDialogParam *, void *);
    int (*result)(SceImeDialogResult *);
    int (*term)(void);
    int (*abort)(void);
    SceImeDialogStatus (*status)(void);
} volatile ime;
static SDL_bool ready;
static SDL_bool active;
static wchar_t text_buffer[0x800];
static const wchar_t dialog_title[0x80] = {0};

_Static_assert(sizeof(SceImeDialogParam) == 96, "PS5 IME parameter ABI");
_Static_assert(sizeof(SceImeDialogResult) == 16, "PS5 IME result ABI");

int PS5_Keyboard_Init(void)
{
    int rc;
    if (ready) return 0;
    rc = sceSysmoduleLoadModule(0x0096);
    if (rc < 0)
        return SDL_SetError("PS5 IME module load: 0x%08x", rc);
    ime.init = sceImeDialogInit;
    ime.result = sceImeDialogGetResult;
    ime.term = sceImeDialogTerm;
    ime.abort = sceImeDialogAbort;
    ime.status = sceImeDialogGetStatus;
    if (!ime.init || !ime.result || !ime.term || !ime.status)
        return SDL_SetError("PS5 IME imports unavailable");
    ready = SDL_TRUE;
    SDL_Log("PS5 on-screen keyboard ready");
    return 0;
}

int PS5_Keyboard_Open(void)
{
    return 0; /* The current foreground user is selected when the dialog opens. */
}

int PS5_Keyboard_Close(void)
{
    if (active) {
        if (ime.abort) ime.abort();
        ime.term();
        active = SDL_FALSE;
    }
    return 0;
}

int PS5_Keyboard_PumpEvents(void)
{
    SceImeDialogResult result = {0};
    if (!ready || !active || ime.status() != SCE_IME_DIALOG_STATUS_FINISHED)
        return 0;
    int rc = ime.result(&result);
    ime.term();
    active = SDL_FALSE;
    if (rc < 0) return SDL_SetError("PS5 IME result: 0x%08x", rc);
    if (result.outcome == SCE_IME_DIALOG_END_STATUS_OK) {
        text_buffer[SDL_arraysize(text_buffer) - 1] = 0;
        char *text = SDL_iconv_string("UTF-8", sizeof(wchar_t) == 4 ? "UTF-32LE" : "UTF-16LE",
                                     (const char *)text_buffer, sizeof(text_buffer));
        if (!text) return SDL_SetError("PS5 IME text conversion failed");
        /* SDL text events have a small buffer. Split on UTF-8 boundaries. */
        const char *remaining = text;
        while (*remaining) {
            char chunk[SDL_TEXTINPUTEVENT_TEXT_SIZE];
            SDL_utf8strlcpy(chunk, remaining, sizeof(chunk));
            size_t length = SDL_strlen(chunk);
            if (!length) break;
            SDL_SendKeyboardText(chunk);
            remaining += length;
        }
        SDL_free(text);
        SDL_SendKeyboardKeyAutoRelease(SDL_SCANCODE_RETURN);
    }
    SDL_memset(text_buffer, 0, sizeof(text_buffer));
    return 0;
}

SDL_bool PS5_HasScreenKeyboardSupport(_THIS)
{
    return ready;
}

void PS5_ShowScreenKeyboard(_THIS, SDL_Window *window)
{
    SceImeDialogParam param = {0};
    if (!ready || active) return;
    int rc = sceUserServiceGetForegroundUser(&param.userId);
    if (rc < 0) {
        SDL_Log("PS5 keyboard foreground user: 0x%08x", rc);
        return;
    }
    SDL_memset(text_buffer, 0, sizeof(text_buffer));
    param.title = dialog_title;
    param.inputTextBuffer = text_buffer;
    param.maxTextLength = SDL_arraysize(text_buffer) - 1;
    rc = ime.init(&param, NULL);
    SDL_Log("PS5 keyboard dialog init: 0x%08x", rc);
    active = rc == 0;
}

void PS5_HideScreenKeyboard(_THIS, SDL_Window *window)
{
    /* The Sony modal takes focus from the game. SDL's focus change must not
     * abort the dialog the user is currently editing. PumpEvents consumes its
     * result; only VideoQuit explicitly aborts an unfinished dialog. */
}

SDL_bool PS5_IsScreenKeyboardShown(_THIS, SDL_Window *window)
{
    return active;
}

#endif /* SDL_VIDEO_DRIVER_PS5 */
