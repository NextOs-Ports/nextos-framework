#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/capture_x86.h"

void debugPrintf(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
}

void *asm2_x86_gl_resolve_now(const char *name)
{
    return SDL_GL_GetProcAddress(name);
}

int main(void)
{
    char request_path[] = "/tmp/asm2-capture-gl-request.XXXXXX";
    char output_path[] = "/tmp/asm2-capture-gl-output.XXXXXX";
    unsigned char ppm[35];
    int request = mkstemp(request_path);
    int output = mkstemp(output_path);
    if (request < 0 || output < 0) {
        perror("mkstemp");
        return 1;
    }
    close(request);
    close(output);
    unlink(output_path);
    setenv("ASM2_CAPTURE_REQUEST", request_path, 1);
    setenv("ASM2_CAPTURE_OUTPUT", output_path, 1);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "ASM2_CAPTURE_GL_TEST_FAIL SDL_Init: %s\n",
                SDL_GetError());
        return 2;
    }
    SDL_Window *window =
        SDL_CreateWindow("capture-test", 0, 0, 4, 2,
                         SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!window) {
        fprintf(stderr, "ASM2_CAPTURE_GL_TEST_FAIL window: %s\n",
                SDL_GetError());
        return 3;
    }
    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (!context) {
        fprintf(stderr, "ASM2_CAPTURE_GL_TEST_FAIL context: %s\n",
                SDL_GetError());
        return 4;
    }

    typedef void (*viewport_fn)(GLint, GLint, GLsizei, GLsizei);
    typedef void (*clear_color_fn)(GLfloat, GLfloat, GLfloat, GLfloat);
    typedef void (*clear_fn)(GLbitfield);
    viewport_fn viewport = (viewport_fn)SDL_GL_GetProcAddress("glViewport");
    clear_color_fn clear_color =
        (clear_color_fn)SDL_GL_GetProcAddress("glClearColor");
    clear_fn clear = (clear_fn)SDL_GL_GetProcAddress("glClear");
    if (!viewport || !clear_color || !clear) {
        fputs("ASM2_CAPTURE_GL_TEST_FAIL resolve clear\n", stderr);
        return 5;
    }
    viewport(0, 0, 4, 2);
    clear_color(0.25f, 0.5f, 0.75f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);
    asm2_x86_capture_backbuffer_if_requested(4, 2);

    struct stat status = {0};
    if (access(request_path, F_OK) == 0 ||
        stat(output_path, &status) != 0 || status.st_size != sizeof(ppm)) {
        fprintf(stderr,
                "ASM2_CAPTURE_GL_TEST_FAIL files request=%d size=%ld\n",
                access(request_path, F_OK), (long)status.st_size);
        return 6;
    }
    FILE *input = fopen(output_path, "rb");
    if (!input || fread(ppm, 1, sizeof(ppm), input) != sizeof(ppm) ||
        fgetc(input) != EOF) {
        fputs("ASM2_CAPTURE_GL_TEST_FAIL read\n", stderr);
        return 7;
    }
    fclose(input);
    unlink(output_path);

    static const unsigned char header[] = "P6\n4 2\n255\n";
    if (memcmp(ppm, header, sizeof(header) - 1u) != 0) {
        fputs("ASM2_CAPTURE_GL_TEST_FAIL header\n", stderr);
        return 8;
    }
    for (size_t offset = sizeof(header) - 1u;
         offset < sizeof(ppm); offset += 3u) {
        if (ppm[offset] < 63 || ppm[offset] > 65 ||
            ppm[offset + 1u] < 127 || ppm[offset + 1u] > 129 ||
            ppm[offset + 2u] < 190 || ppm[offset + 2u] > 192) {
            fprintf(stderr,
                    "ASM2_CAPTURE_GL_TEST_FAIL pixel=%u/%u/%u\n",
                    ppm[offset], ppm[offset + 1u], ppm[offset + 2u]);
            return 9;
        }
    }

    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    fprintf(stderr,
            "ASM2_CAPTURE_GL_TEST_OK bytes=%zu rgb=%u/%u/%u\n",
            sizeof(ppm), ppm[sizeof(header) - 1u],
            ppm[sizeof(header)], ppm[sizeof(header) + 1u]);
    return 0;
}
