/* nx_common.c — shared by both shim libraries (each gets its own copy). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void nx_log(const char *fmt, ...)
{
    static FILE *f;
    static int tried;
    if (!f && !tried) {
        tried = 1;
        const char *p = getenv("NX_SHIM_LOG");
        f = fopen(p ? p : "/tmp/nx_shim.log", "a");
        if (f) setvbuf(f, NULL, _IOLBF, 0);
    }
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
}

static void *load(const char *a, const char *b)
{
    void *h = dlopen(a, RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen(b, RTLD_NOW | RTLD_LOCAL);
    return h;
}

void *nx_mali_egl_sym(const char *name)
{
    static void *mali_egl;
    if (!mali_egl)
        mali_egl = load("libEGL.so.1", "/usr/lib/libEGL.so.1");
    return mali_egl ? dlsym(mali_egl, name) : NULL;
}

void *nx_mali_sym(const char *name)
{
    static void *mali_gles;
    if (!mali_gles)
        mali_gles = load("libGLESv2.so.2", "/usr/lib/libGLESv2.so.2");
    return mali_gles ? dlsym(mali_gles, name) : NULL;
}
