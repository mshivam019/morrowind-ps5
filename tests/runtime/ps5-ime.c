/* Exercise the native IME adapter against SDL's real UTF conversion, with
 * deterministic system-dialog replies. No console or game data is required. */
#include <assert.h>
#include <string.h>
#include "../../source-overlays/deps/sdl2-compat/integration/SDL_ps5keyboard.c"

static int module_rc, dialog_rc, result_rc, outcome;
static int opens, terms, aborts, enters, chunks;
static SceImeDialogStatus status;
static SceImeDialogParam shown;
static char received[2048];
int sceSysmoduleLoadModule(uint16_t id) { assert(id == 0x96); return module_rc; }
int sceUserServiceGetForegroundUser(int *id) { *id = 42; return 0; }
int sceImeDialogInit(const SceImeDialogParam *param, void *extra)
{
    assert(!extra && param->userId == 42 && param->maxTextLength == 2047);
    shown = *param; ++opens; return dialog_rc;
}
int sceImeDialogGetResult(SceImeDialogResult *result)
{ result->outcome = outcome; return result_rc; }
int sceImeDialogTerm(void) { ++terms; return 0; }
int sceImeDialogAbort(void) { ++aborts; return 0; }
SceImeDialogStatus sceImeDialogGetStatus(void) { return status; }
int SDL_SendKeyboardText(const char *text)
{
    assert(strlen(text) < SDL_TEXTINPUTEVENT_TEXT_SIZE);
    strcat(received, text); ++chunks; return 1;
}
int SDL_SendKeyboardKeyAutoRelease(SDL_Scancode code)
{ assert(code == SDL_SCANCODE_RETURN); ++enters; return 1; }
int main(void)
{
    module_rc = -1;
    assert(PS5_Keyboard_Init() < 0);
    PS5_ShowScreenKeyboard(NULL, NULL);
    assert(opens == 0);
    module_rc = 0;
    assert(PS5_Keyboard_Init() == 0);
    assert(PS5_HasScreenKeyboardSupport(NULL));
    dialog_rc = -1;
    PS5_ShowScreenKeyboard(NULL, NULL);
    assert(!PS5_IsScreenKeyboardShown(NULL, NULL));
    dialog_rc = 0;
    PS5_ShowScreenKeyboard(NULL, NULL);
    PS5_ShowScreenKeyboard(NULL, NULL);
    assert(opens == 2 && PS5_IsScreenKeyboardShown(NULL, NULL));
    /* Cross an SDL event boundary with non-ASCII native wide characters. */
    for (int i = 0; i < 30; ++i) shown.inputTextBuffer[i] = 'a';
    shown.inputTextBuffer[30] = 0xe9;
    shown.inputTextBuffer[31] = 0x1f600;
    status = SCE_IME_DIALOG_STATUS_RUNNING;
    PS5_Keyboard_PumpEvents(); assert(chunks == 0);
    status = SCE_IME_DIALOG_STATUS_FINISHED;
    PS5_Keyboard_PumpEvents();
    assert(strlen(received) == 36 && chunks == 2 && enters == 1 && terms == 1);
    assert(strcmp(received + 30, "\xc3\xa9\xf0\x9f\x98\x80") == 0);
    PS5_Keyboard_PumpEvents(); assert(enters == 1 && terms == 1);
    PS5_ShowScreenKeyboard(NULL, NULL);
    outcome = SCE_IME_DIALOG_END_STATUS_USER_CANCELED;
    PS5_Keyboard_PumpEvents(); assert(enters == 1 && terms == 2);
    PS5_ShowScreenKeyboard(NULL, NULL);
    result_rc = -1;
    assert(PS5_Keyboard_PumpEvents() < 0 && terms == 3);
    PS5_ShowScreenKeyboard(NULL, NULL);
    PS5_HideScreenKeyboard(NULL, NULL);
    assert(active && aborts == 0); /* Native dialog temporarily owns focus. */
    PS5_Keyboard_Close();
    assert(aborts == 1 && terms == 4 && !active);
    puts("PS5 IME lifecycle and wide-character tests passed");
}
