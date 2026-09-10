/*
 * imports.c -- the symbols we serve ourselves.
 *
 * so_resolve falls back to dlsym(RTLD_DEFAULT) for anything not listed here,
 * and the device's glibc 2.43 / libGLESv2 / libstdc++ cover ~450 of the guest's
 * 458 undefined symbols.  This table is only the genuine Bionic/Android gap
 * plus the two GL entry points we need to scale (see egl_shim.c).
 */
#include <stdint.h>

#include "egl_shim.h"
#include "gl_state.h"
#include "so_util.h"

/* bionic_shims.c */
extern unsigned char __sF[];
extern uintptr_t gd_stack_chk_guard;
extern size_t gd_fwrite(const void *, size_t, size_t, void *);
extern size_t gd_fread(void *, size_t, size_t, void *);
extern int gd_fputs(const char *, void *);
extern int gd_fputc(int, void *);
extern int gd_ungetc(int, void *);
extern int gd_feof(void *);
extern int gd_ferror(void *);
extern int gd_fileno(void *);
extern int gd_fseek(void *, long, int);
extern long gd_ftell(void *);
extern int gd_fseeko(void *, long, int);
extern long gd_ftello(void *);
extern char *gd_fgets(char *, int, void *);
extern int gd_fflush(void *);
extern int gd_fclose(void *);
extern int gd_fprintf(void *, const char *, ...);
extern int gd_vfprintf(void *, const char *, void *);
extern int *gd_errno(void);
extern int __android_log_print(int, const char *, const char *, ...);
extern int __android_log_write(int, const char *, const char *);
extern int __android_log_vprint(int, const char *, const char *, void *);
extern void android_set_abort_message(const char *);
extern void gd_assert2(const char *, int, const char *, const char *);
extern void gd_stack_chk_fail(void);
extern int gd_sigaction(int, const void *, void *);
extern int gd_setjmp(void *);
extern void gd_longjmp(void *, int);
extern int gd_sigsetjmp(void *, int);
extern void gd_siglongjmp(void *, int);
extern int gd_sem_init(void *, int, unsigned);
extern int gd_sem_destroy(void *);
extern int gd_sem_wait(void *);
extern int gd_sem_trywait(void *);
extern int gd_sem_post(void *);
extern int gd_sem_getvalue(void *, int *);
extern int gd_gettid(void);
extern void gd_FD_SET_chk(int, void *, size_t);
extern void *gd_dlopen(const char *, int);
extern void *gd_dlsym(void *, const char *);
extern int gd_dlclose(void *);
extern char *gd_dlerror(void);
extern int gd_pthread_key_create(unsigned *, void (*)(void *));
extern int gd_gettimeofday(void *, void *);
/* save_redirect.c: the guest's /data/data/<package>/ is our userdata dir. */
extern void *gd_fopen(const char *, const char *);
extern int gd_open(const char *, int, ...);
extern int gd_rename(const char *, const char *);
extern int gd_remove(const char *);
extern int gd_mkdir(const char *, unsigned);
extern int gd_access(const char *, int);
extern int gd_chmod(const char *, unsigned);
extern void *gd_opendir(const char *);
extern int gd_stat(const char *, void *);
extern int gd_getc(void *);
extern int gd_putc(int, void *);
extern void gd_setbuf(void *, char *);
extern int gd_setvbuf(void *, char *, int, size_t);
extern unsigned gd_getwc(void *);
extern unsigned gd_putwc(int, void *);
extern unsigned gd_ungetwc(unsigned, void *);
extern const unsigned char *_ctype_;
extern int gd_fpclassifyd(double);
extern void gd_blocking_region_begin(void);
extern void gd_blocking_region_end(void);

DynLibFunction gd_overrides[] = {
    /* stdio: Bionic's __sF pointers are not glibc FILE* */
    {"__sF", (uintptr_t)&__sF},
    {"fwrite", (uintptr_t)&gd_fwrite},
    {"fread", (uintptr_t)&gd_fread},
    {"fputs", (uintptr_t)&gd_fputs},
    {"fputc", (uintptr_t)&gd_fputc},
    {"ungetc", (uintptr_t)&gd_ungetc},
    {"feof", (uintptr_t)&gd_feof},
    {"ferror", (uintptr_t)&gd_ferror},
    {"fileno", (uintptr_t)&gd_fileno},
    {"fseek", (uintptr_t)&gd_fseek},
    {"ftell", (uintptr_t)&gd_ftell},
    {"fseeko", (uintptr_t)&gd_fseeko},
    {"ftello", (uintptr_t)&gd_ftello},
    {"fgets", (uintptr_t)&gd_fgets},
    {"fflush", (uintptr_t)&gd_fflush},
    {"fclose", (uintptr_t)&gd_fclose},
    {"fprintf", (uintptr_t)&gd_fprintf},
    {"vfprintf", (uintptr_t)&gd_vfprintf},

    {"fopen", (uintptr_t)&gd_fopen},
    {"open", (uintptr_t)&gd_open},
    {"rename", (uintptr_t)&gd_rename},
    {"remove", (uintptr_t)&gd_remove},
    {"mkdir", (uintptr_t)&gd_mkdir},
    {"access", (uintptr_t)&gd_access},
    {"chmod", (uintptr_t)&gd_chmod},
    {"opendir", (uintptr_t)&gd_opendir},
    {"stat", (uintptr_t)&gd_stat},
    {"getc", (uintptr_t)&gd_getc},
    {"putc", (uintptr_t)&gd_putc},
    {"setbuf", (uintptr_t)&gd_setbuf},
    {"setvbuf", (uintptr_t)&gd_setvbuf},
    {"getwc", (uintptr_t)&gd_getwc},
    {"putwc", (uintptr_t)&gd_putwc},
    {"ungetwc", (uintptr_t)&gd_ungetwc},

    /* Bionic-only libc surface this build reaches for and SubZero did not. */
    {"_ctype_", (uintptr_t)&_ctype_},
    {"__fpclassifyd", (uintptr_t)&gd_fpclassifyd},
    {"__google_potentially_blocking_region_begin",
     (uintptr_t)&gd_blocking_region_begin},
    {"__google_potentially_blocking_region_end",
     (uintptr_t)&gd_blocking_region_end},

    {"__errno", (uintptr_t)&gd_errno},

    {"__android_log_print", (uintptr_t)&__android_log_print},
    {"__android_log_write", (uintptr_t)&__android_log_write},
    {"__android_log_vprint", (uintptr_t)&__android_log_vprint},
    {"android_set_abort_message", (uintptr_t)&android_set_abort_message},
    {"__assert2", (uintptr_t)&gd_assert2},

    {"__stack_chk_guard", (uintptr_t)&gd_stack_chk_guard},
    {"__stack_chk_fail", (uintptr_t)&gd_stack_chk_fail},

    /* Bionic's LP64 struct sigaction starts with sa_flags -- different type. */
    {"sigaction", (uintptr_t)&gd_sigaction},

    {"setjmp", (uintptr_t)&gd_setjmp},
    {"_setjmp", (uintptr_t)&gd_setjmp},
    {"longjmp", (uintptr_t)&gd_longjmp},
    {"_longjmp", (uintptr_t)&gd_longjmp},
    {"sigsetjmp", (uintptr_t)&gd_sigsetjmp},
    {"siglongjmp", (uintptr_t)&gd_siglongjmp},

    /* Bionic sem_t is one int; glibc's is 32 bytes. */
    {"sem_init", (uintptr_t)&gd_sem_init},
    {"sem_destroy", (uintptr_t)&gd_sem_destroy},
    {"sem_wait", (uintptr_t)&gd_sem_wait},
    {"sem_trywait", (uintptr_t)&gd_sem_trywait},
    {"sem_post", (uintptr_t)&gd_sem_post},
    {"sem_getvalue", (uintptr_t)&gd_sem_getvalue},

    {"gettid", (uintptr_t)&gd_gettid},
    {"__FD_SET_chk", (uintptr_t)&gd_FD_SET_chk},

    {"dlopen", (uintptr_t)&gd_dlopen},
    {"dlsym", (uintptr_t)&gd_dlsym},
    {"dlclose", (uintptr_t)&gd_dlclose},
    {"dlerror", (uintptr_t)&gd_dlerror},

    {"pthread_key_create", (uintptr_t)&gd_pthread_key_create},

    /* The engine's frame clock -- monotonic, and never allowed to jump. */
    {"gettimeofday", (uintptr_t)&gd_gettimeofday},

    /* GL state: every call the engine makes that the cursor overlay also
     * touches goes through gl_state.c, which mirrors it.  That is what lets the
     * overlay put the state back without a single glGet* -- and without calling
     * ccGLInvalidateStateCache(), which would free the projection matrix.
     * glViewport/glScissor additionally scale engine space to the panel. */
    {"glViewport", (uintptr_t)&gd_glViewport},
    {"glScissor", (uintptr_t)&gd_glScissor},
    {"glUseProgram", (uintptr_t)&gd_glUseProgram},
    {"glActiveTexture", (uintptr_t)&gd_glActiveTexture},
    {"glBindTexture", (uintptr_t)&gd_glBindTexture},
    {"glBindBuffer", (uintptr_t)&gd_glBindBuffer},
    {"glBindFramebuffer", (uintptr_t)&gd_glBindFramebuffer},
    {"glBlendFunc", (uintptr_t)&gd_glBlendFunc},
    {"glEnable", (uintptr_t)&gd_glEnable},
    {"glDisable", (uintptr_t)&gd_glDisable},
    {"glEnableVertexAttribArray", (uintptr_t)&gd_glEnableVertexAttribArray},
    {"glDisableVertexAttribArray", (uintptr_t)&gd_glDisableVertexAttribArray},
    {"glVertexAttribPointer", (uintptr_t)&gd_glVertexAttribPointer},
};
const int gd_overrides_count =
    sizeof(gd_overrides) / sizeof(gd_overrides[0]);
