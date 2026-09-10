// SPDX-License-Identifier: GPL-3.0-only
/*
 * Freestanding AArch64 late-binding probe for the ROCKNIX PC environment.
 * It opens the target EGL/GLES dispatch libraries and resolves symbols only;
 * it never calls EGL, creates a context, or opens a display/device.
 */

#define RTLD_NOW 2

extern void *dlopen(const char *name, int flags);
extern void *dlsym(void *handle, const char *name);
extern int dlclose(void *handle);

static unsigned long text_length(const char *text)
{
    unsigned long length = 0;

    while (text[length] != '\0')
        ++length;
    return length;
}

static void write_text(const char *text)
{
    register long x0 __asm__("x0") = 1;
    register const char *x1 __asm__("x1") = text;
    register unsigned long x2 __asm__("x2") = text_length(text);
    register long x8 __asm__("x8") = 64;

    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x8)
                     : "memory");
}

static void exit_process(int status)
{
    register long x0 __asm__("x0") = status;
    register long x8 __asm__("x8") = 93;

    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    for (;;)
        ;
}

void _start(void)
{
    void *egl = dlopen("libEGL.so.1", RTLD_NOW);
    void *gles = dlopen("libGLESv2.so.2", RTLD_NOW);

    if (!egl || !gles ||
        !dlsym(egl, "eglGetDisplay") || !dlsym(egl, "eglInitialize") ||
        !dlsym(gles, "glCreateShader") || !dlsym(gles, "glGetString")) {
        write_text("late-bind=failed\n");
        exit_process(1);
    }
    if (dlclose(gles) != 0 || dlclose(egl) != 0) {
        write_text("late-bind=close-failed\n");
        exit_process(1);
    }
    write_text("late-bind=EGL,GLES2 symbols-only\n");
    exit_process(0);
}
