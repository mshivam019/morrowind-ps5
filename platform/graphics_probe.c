/* Native compatibility-context probe; not the game executable. */
#include <SDL.h>
#include <GL/gl.h>
#include <stdio.h>
#include <sys/stat.h>
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    freopen("/download0/graphics-probe.log", "w", stdout);
    setvbuf(stdout, NULL, _IONBF, 0);
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
        printf("SDL_Init: %s\n", SDL_GetError()); return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_DisplayMode mode;
    if (SDL_GetDesktopDisplayMode(0, &mode) != 0) return 2;
    SDL_Window *w=SDL_CreateWindow("Morrowind graphics probe",0,0,mode.w,mode.h,SDL_WINDOW_OPENGL);
    if (!w) { printf("window: %s\n", SDL_GetError()); return 3; }
    SDL_GLContext c=SDL_GL_CreateContext(w);
    if (!c) { printf("context: %s\n", SDL_GetError()); return 4; }
    printf("GL=%s GLSL=%s size=%dx%d controllers=%d audio=%s\n",glGetString(GL_VERSION),glGetString(GL_SHADING_LANGUAGE_VERSION),mode.w,mode.h,SDL_NumJoysticks(),SDL_GetCurrentAudioDriver());
    for (int i=0;i<SDL_NumJoysticks();++i) printf("pad %d gamecontroller=%d name=%s\n",i,SDL_IsGameController(i),SDL_JoystickNameForIndex(i));
    for (int frame=0;frame<600;++frame) {
        SDL_Event event; while (SDL_PollEvent(&event)) if (event.type==SDL_QUIT) return 0;
        glViewport(0,0,mode.w,mode.h);
        glClearColor(0.06f,0.12f,0.2f,1.0f); glClear(GL_COLOR_BUFFER_BIT);
        glMatrixMode(GL_PROJECTION);glLoadIdentity();glMatrixMode(GL_MODELVIEW);glLoadIdentity();
        glBegin(GL_TRIANGLES);glColor3f(1,0,0);glVertex2f(-0.6f,-0.5f);glColor3f(0,1,0);glVertex2f(0.6f,-0.5f);glColor3f(0,0,1);glVertex2f(0,0.6f);glEnd();
        if (frame == 0 || frame == 60) { unsigned char p[4] = {99,99,99,99};glFinish();glReadBuffer(GL_BACK);glReadPixels(mode.w/2,mode.h/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,p); printf("frame=%d center=%u,%u,%u,%u error=0x%x\n",frame,p[0],p[1],p[2],p[3],glGetError());glReadPixels(4,4,1,1,GL_RGBA,GL_UNSIGNED_BYTE,p);printf("background=%u,%u,%u,%u error=0x%x\n",p[0],p[1],p[2],p[3],glGetError()); }
        SDL_GL_SwapWindow(w);
    }
    printf("probe completed\n");SDL_GL_DeleteContext(c);SDL_DestroyWindow(w);SDL_Quit();return 0;
}
