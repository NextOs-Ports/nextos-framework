/* Explicit compatibility imports for the clean-room 1.2.7d loader.
 *
 * This table always wins over dlsym.  It contains every symbol whose Android
 * ARM32 representation or base-PCS ABI differs from the ARM hard-float host,
 * plus platform APIs that do not exist on NextOS.  Plain integer/pointer libc
 * and GLES2 calls are deliberately left to the host fallback.
 */
#include "imports.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__i386__)
#include <signal.h>
#endif

#include "bionic_compat.h"
#include "opensl_bridge.h"
#include "platform_shims.h"
#include "pthread_bridge.h"
#if defined(__arm__)
#include "softfp_bridge.h"
#elif defined(__i386__)
#include "x86_runtime_compat.h"
#endif

extern int asm2_setjmp(void *buffer);
extern void asm2_longjmp(void *buffer, int value) __attribute__((noreturn));

#define ASM2_IMPORT(symbol_name, target) \
  { symbol_name, (uintptr_t)(target) }

DynLibFunction dynlib_functions[] = {
    /* Android/Bionic data and functions. */
    ASM2_IMPORT("_ctype_", &asm2_ctype_ptr),
    ASM2_IMPORT("_tolower_tab_", &asm2_tolower_ptr),
    ASM2_IMPORT("_toupper_tab_", &asm2_toupper_ptr),
    ASM2_IMPORT("__page_size", &asm2_page_size),
    ASM2_IMPORT("__stack_chk_guard", &asm2_stack_chk_guard),
    ASM2_IMPORT("__sF", &asm2_bionic_sF[0][0]),
    ASM2_IMPORT("__errno", asm2_errno),
    ASM2_IMPORT("__android_log_print", asm2_android_log_print),
    ASM2_IMPORT("__android_log_vprint", asm2_android_log_vprint),
    ASM2_IMPORT("__assert2", asm2_assert2),
#if defined(__arm__)
    ASM2_IMPORT("__gnu_Unwind_Find_exidx", asm2_unwind_find_exidx),
#elif defined(__i386__)
    ASM2_IMPORT("__cxa_atexit", asm2_x86_cxa_atexit),
    ASM2_IMPORT("dl_iterate_phdr", asm2_x86_dl_iterate_phdr),
    /* Box32 does not export glibc's bsd_signal alias through its fallback
     * dlsym path.  Linux signal() has the same i386 SysV signature. */
    ASM2_IMPORT("bsd_signal", signal),
#endif

    /* Android FILE is 84 bytes on ARM32; never pass __sF to glibc. */
    ASM2_IMPORT("clearerr", asm2_clearerr),
    ASM2_IMPORT("fclose", asm2_fclose),
    ASM2_IMPORT("fdopen", asm2_fdopen),
    ASM2_IMPORT("fflush", asm2_fflush),
    ASM2_IMPORT("fgets", asm2_fgets),
    ASM2_IMPORT("fopen", asm2_fopen),
    ASM2_IMPORT("fprintf", asm2_fprintf),
    ASM2_IMPORT("fputc", asm2_fputc),
    ASM2_IMPORT("fputs", asm2_fputs),
    ASM2_IMPORT("fread", asm2_fread),
    ASM2_IMPORT("fseek", asm2_fseek),
    ASM2_IMPORT("fseeko", asm2_fseeko),
    ASM2_IMPORT("ftell", asm2_ftell),
    ASM2_IMPORT("ftello", asm2_ftello),
    ASM2_IMPORT("fwrite", asm2_fwrite),
    ASM2_IMPORT("getc", asm2_getc),
    ASM2_IMPORT("getwc", asm2_getwc),
    ASM2_IMPORT("putc", asm2_putc),
    ASM2_IMPORT("putwc", asm2_putwc),
    ASM2_IMPORT("setvbuf", asm2_setvbuf),
    ASM2_IMPORT("ungetc", asm2_ungetc),
    ASM2_IMPORT("ungetwc", asm2_ungetwc),
    ASM2_IMPORT("vfprintf", asm2_vfprintf),

    /* Path virtualization and Android-vs-glibc structure layouts. */
    ASM2_IMPORT("access", asm2_access),
    ASM2_IMPORT("chdir", asm2_chdir),
    ASM2_IMPORT("chmod", asm2_chmod),
    ASM2_IMPORT("getcwd", asm2_getcwd),
    ASM2_IMPORT("mkdir", asm2_mkdir),
    ASM2_IMPORT("mkstemp", asm2_mkstemp),
    ASM2_IMPORT("open", asm2_open),
    ASM2_IMPORT("opendir", asm2_opendir),
    ASM2_IMPORT("readdir", asm2_readdir),
    ASM2_IMPORT("remove", asm2_remove),
    ASM2_IMPORT("rename", asm2_rename),
    ASM2_IMPORT("rmdir", asm2_rmdir),
    ASM2_IMPORT("stat", asm2_stat),
    ASM2_IMPORT("fstat", asm2_fstat),
    ASM2_IMPORT("statfs", asm2_statfs),
    ASM2_IMPORT("unlink", asm2_unlink),
    ASM2_IMPORT("sigaction", asm2_sigaction),
    ASM2_IMPORT("strerror_r", asm2_strerror_r),
    ASM2_IMPORT("sysconf", asm2_sysconf),
    ASM2_IMPORT("setjmp", asm2_setjmp),
    ASM2_IMPORT("longjmp", asm2_longjmp),

    /* pthread objects are much smaller in old Bionic than in glibc. */
    ASM2_IMPORT("pthread_attr_init", asm2_pthread_attr_init),
    ASM2_IMPORT("pthread_attr_destroy", asm2_pthread_attr_destroy),
    ASM2_IMPORT("pthread_attr_getdetachstate", asm2_pthread_attr_getdetachstate),
    ASM2_IMPORT("pthread_attr_setdetachstate", asm2_pthread_attr_setdetachstate),
    ASM2_IMPORT("pthread_attr_setstacksize", asm2_pthread_attr_setstacksize),
    ASM2_IMPORT("pthread_mutexattr_init", asm2_pthread_mutexattr_init),
    ASM2_IMPORT("pthread_mutexattr_destroy", asm2_pthread_mutexattr_destroy),
    ASM2_IMPORT("pthread_mutexattr_settype", asm2_pthread_mutexattr_settype),
    ASM2_IMPORT("pthread_mutex_init", asm2_pthread_mutex_init),
    ASM2_IMPORT("pthread_mutex_destroy", asm2_pthread_mutex_destroy),
    ASM2_IMPORT("pthread_mutex_lock", asm2_pthread_mutex_lock),
    ASM2_IMPORT("pthread_mutex_trylock", asm2_pthread_mutex_trylock),
    ASM2_IMPORT("pthread_mutex_unlock", asm2_pthread_mutex_unlock),
    ASM2_IMPORT("pthread_cond_init", asm2_pthread_cond_init),
    ASM2_IMPORT("pthread_cond_destroy", asm2_pthread_cond_destroy),
    ASM2_IMPORT("pthread_cond_wait", asm2_pthread_cond_wait),
    ASM2_IMPORT("pthread_cond_timedwait", asm2_pthread_cond_timedwait),
    ASM2_IMPORT("pthread_cond_signal", asm2_pthread_cond_signal),
    ASM2_IMPORT("pthread_cond_broadcast", asm2_pthread_cond_broadcast),
    ASM2_IMPORT("pthread_create", asm2_pthread_create),
    ASM2_IMPORT("pthread_detach", asm2_pthread_detach),
    ASM2_IMPORT("pthread_equal", asm2_pthread_equal),
    ASM2_IMPORT("pthread_getschedparam", asm2_pthread_getschedparam),
    ASM2_IMPORT("pthread_setschedparam", asm2_pthread_setschedparam),
    ASM2_IMPORT("pthread_getspecific", asm2_pthread_getspecific),
    ASM2_IMPORT("pthread_setspecific", asm2_pthread_setspecific),
    ASM2_IMPORT("pthread_join", asm2_pthread_join),
    ASM2_IMPORT("pthread_key_create", asm2_pthread_key_create),
    ASM2_IMPORT("pthread_key_delete", asm2_pthread_key_delete),
    ASM2_IMPORT("pthread_self", asm2_pthread_self),
    ASM2_IMPORT("pthread_once", asm2_pthread_once),
    ASM2_IMPORT("sem_init", asm2_sem_init),
    ASM2_IMPORT("sem_destroy", asm2_sem_destroy),
    ASM2_IMPORT("sem_post", asm2_sem_post),
    ASM2_IMPORT("sem_trywait", asm2_sem_trywait),
    ASM2_IMPORT("sem_wait", asm2_sem_wait),

    /* ARM base-PCS guest to hard-float host bridges.  Android x86 and Linux
     * i386 are both SysV cdecl, so the x86 loader resolves libm directly. */
#if defined(__arm__)
    ASM2_IMPORT("acos", asm2_sf_acos),
    ASM2_IMPORT("acosf", asm2_sf_acosf),
    ASM2_IMPORT("asin", asm2_sf_asin),
    ASM2_IMPORT("asinf", asm2_sf_asinf),
    ASM2_IMPORT("atan", asm2_sf_atan),
    ASM2_IMPORT("atanf", asm2_sf_atanf),
    ASM2_IMPORT("atan2", asm2_sf_atan2),
    ASM2_IMPORT("atan2f", asm2_sf_atan2f),
    ASM2_IMPORT("ceil", asm2_sf_ceil),
    ASM2_IMPORT("ceilf", asm2_sf_ceilf),
    ASM2_IMPORT("cos", asm2_sf_cos),
    ASM2_IMPORT("cosf", asm2_sf_cosf),
    ASM2_IMPORT("difftime", asm2_sf_difftime),
    ASM2_IMPORT("exp", asm2_sf_exp),
    ASM2_IMPORT("expf", asm2_sf_expf),
    ASM2_IMPORT("floor", asm2_sf_floor),
    ASM2_IMPORT("floorf", asm2_sf_floorf),
    ASM2_IMPORT("fmod", asm2_sf_fmod),
    ASM2_IMPORT("fmodf", asm2_sf_fmodf),
    ASM2_IMPORT("ldexp", asm2_sf_ldexp),
    ASM2_IMPORT("log", asm2_sf_log),
    ASM2_IMPORT("logf", asm2_sf_logf),
    ASM2_IMPORT("log10", asm2_sf_log10),
    ASM2_IMPORT("modff", asm2_sf_modff),
    ASM2_IMPORT("pow", asm2_sf_pow),
    ASM2_IMPORT("powf", asm2_sf_powf),
    ASM2_IMPORT("sin", asm2_sf_sin),
    ASM2_IMPORT("sinf", asm2_sf_sinf),
    ASM2_IMPORT("sinh", asm2_sf_sinh),
    ASM2_IMPORT("strtod", asm2_sf_strtod),
    ASM2_IMPORT("tan", asm2_sf_tan),
    ASM2_IMPORT("tanf", asm2_sf_tanf),
    ASM2_IMPORT("glBlendColor", asm2_sf_glBlendColor),
    ASM2_IMPORT("glClearColor", asm2_sf_glClearColor),
    ASM2_IMPORT("glClearDepthf", asm2_sf_glClearDepthf),
    ASM2_IMPORT("glDepthRangef", asm2_sf_glDepthRangef),
    ASM2_IMPORT("glLineWidth", asm2_sf_glLineWidth),
    ASM2_IMPORT("glPolygonOffset", asm2_sf_glPolygonOffset),
    ASM2_IMPORT("glSampleCoverage", asm2_sf_glSampleCoverage),
    ASM2_IMPORT("glTexParameterf", asm2_sf_glTexParameterf),
    ASM2_IMPORT("glUniform1f", asm2_sf_glUniform1f),
    ASM2_IMPORT("glVertexAttrib4f", asm2_sf_glVertexAttrib4f),
    /*
     * Some Mesa/ROCKNIX GLES stacks expose OES_mapbuffer only through the
     * active SDL/EGL procedure resolver, or expose the GLES3 core names
     * without the OES suffix.  Bind guest imports to lazy wrappers so strong
     * relocation can finish before the SDL context exists.
     */
    ASM2_IMPORT("glMapBufferOES", asm2_glMapBufferOES),
    ASM2_IMPORT("glUnmapBufferOES", asm2_glUnmapBufferOES),
#elif defined(__i386__)
    /*
     * Box32 has no dedicated libGLESv2 wrapper on the X5M route.  Each import
     * receives a signature-transparent i386 lazy tail-jump which asks
     * SDL_GL_GetProcAddress only on first use, after SDL has made the native
     * EGL context current.  Extension/fence symbols below retain their
     * compatibility shims and are intentionally omitted from this list.
     */
    ASM2_IMPORT("glActiveTexture", 0),
    ASM2_IMPORT("glAttachShader", 0),
    ASM2_IMPORT("glBindBuffer", 0),
    ASM2_IMPORT("glBindFramebuffer", 0),
    ASM2_IMPORT("glBindRenderbuffer", 0),
    ASM2_IMPORT("glBindTexture", 0),
    ASM2_IMPORT("glBlendColor", 0),
    ASM2_IMPORT("glBlendEquation", 0),
    ASM2_IMPORT("glBlendEquationSeparate", 0),
    ASM2_IMPORT("glBlendFunc", 0),
    ASM2_IMPORT("glBlendFuncSeparate", 0),
    ASM2_IMPORT("glBufferData", 0),
    ASM2_IMPORT("glBufferSubData", 0),
    ASM2_IMPORT("glCheckFramebufferStatus", 0),
    ASM2_IMPORT("glClear", 0),
    ASM2_IMPORT("glClearColor", 0),
    ASM2_IMPORT("glClearDepthf", 0),
    ASM2_IMPORT("glClearStencil", 0),
    ASM2_IMPORT("glColorMask", 0),
    ASM2_IMPORT("glCompileShader", 0),
    ASM2_IMPORT("glCompressedTexImage2D", 0),
    ASM2_IMPORT("glCompressedTexSubImage2D", 0),
    ASM2_IMPORT("glCopyTexSubImage2D", 0),
    ASM2_IMPORT("glCreateProgram", 0),
    ASM2_IMPORT("glCreateShader", 0),
    ASM2_IMPORT("glCullFace", 0),
    ASM2_IMPORT("glDeleteBuffers", 0),
    ASM2_IMPORT("glDeleteFramebuffers", 0),
    ASM2_IMPORT("glDeleteProgram", 0),
    ASM2_IMPORT("glDeleteRenderbuffers", 0),
    ASM2_IMPORT("glDeleteShader", 0),
    ASM2_IMPORT("glDeleteTextures", 0),
    ASM2_IMPORT("glDepthFunc", 0),
    ASM2_IMPORT("glDepthMask", 0),
    ASM2_IMPORT("glDepthRangef", 0),
    ASM2_IMPORT("glDisable", 0),
    ASM2_IMPORT("glDisableVertexAttribArray", 0),
    ASM2_IMPORT("glDrawArrays", 0),
    ASM2_IMPORT("glDrawElements", 0),
    ASM2_IMPORT("glEnable", 0),
    ASM2_IMPORT("glEnableVertexAttribArray", 0),
    ASM2_IMPORT("glFlush", 0),
    ASM2_IMPORT("glFramebufferRenderbuffer", 0),
    ASM2_IMPORT("glFramebufferTexture2D", 0),
    ASM2_IMPORT("glFrontFace", 0),
    ASM2_IMPORT("glGenBuffers", 0),
    ASM2_IMPORT("glGenerateMipmap", 0),
    ASM2_IMPORT("glGenFramebuffers", 0),
    ASM2_IMPORT("glGenRenderbuffers", 0),
    ASM2_IMPORT("glGenTextures", 0),
    ASM2_IMPORT("glGetActiveAttrib", 0),
    ASM2_IMPORT("glGetActiveUniform", 0),
    ASM2_IMPORT("glGetAttribLocation", 0),
    ASM2_IMPORT("glGetError", 0),
    ASM2_IMPORT("glGetFloatv", 0),
    ASM2_IMPORT("glGetIntegerv", 0),
    ASM2_IMPORT("glGetProgramInfoLog", 0),
    ASM2_IMPORT("glGetProgramiv", 0),
    ASM2_IMPORT("glGetShaderInfoLog", 0),
    ASM2_IMPORT("glGetShaderiv", 0),
    ASM2_IMPORT("glGetShaderSource", 0),
    ASM2_IMPORT("glGetString", 0),
    ASM2_IMPORT("glGetUniformLocation", 0),
    ASM2_IMPORT("glLineWidth", 0),
    ASM2_IMPORT("glLinkProgram", 0),
    ASM2_IMPORT("glMapBufferOES", 0),
    ASM2_IMPORT("glPixelStorei", 0),
    ASM2_IMPORT("glPolygonOffset", 0),
    ASM2_IMPORT("glReadPixels", 0),
    ASM2_IMPORT("glRenderbufferStorage", 0),
    ASM2_IMPORT("glSampleCoverage", 0),
    ASM2_IMPORT("glScissor", 0),
    ASM2_IMPORT("glShaderSource", 0),
    ASM2_IMPORT("glStencilFunc", 0),
    ASM2_IMPORT("glStencilMask", 0),
    ASM2_IMPORT("glStencilOp", 0),
    ASM2_IMPORT("glTexImage2D", 0),
    ASM2_IMPORT("glTexParameterf", 0),
    ASM2_IMPORT("glTexParameteri", 0),
    ASM2_IMPORT("glTexSubImage2D", 0),
    ASM2_IMPORT("glUniform1f", 0),
    ASM2_IMPORT("glUniform1fv", 0),
    ASM2_IMPORT("glUniform1i", 0),
    ASM2_IMPORT("glUniform1iv", 0),
    ASM2_IMPORT("glUniform2fv", 0),
    ASM2_IMPORT("glUniform2iv", 0),
    ASM2_IMPORT("glUniform3fv", 0),
    ASM2_IMPORT("glUniform3iv", 0),
    ASM2_IMPORT("glUniform4fv", 0),
    ASM2_IMPORT("glUniform4iv", 0),
    ASM2_IMPORT("glUniformMatrix2fv", 0),
    ASM2_IMPORT("glUniformMatrix3fv", 0),
    ASM2_IMPORT("glUniformMatrix4fv", 0),
    ASM2_IMPORT("glUnmapBufferOES", 0),
    ASM2_IMPORT("glUseProgram", 0),
    ASM2_IMPORT("glVertexAttrib4f", 0),
    ASM2_IMPORT("glVertexAttribPointer", 0),
    ASM2_IMPORT("glViewport", 0),
#endif

    /* Preserve glFinish semantics while counting explicit guest stalls. */
    ASM2_IMPORT("glFinish", asm2_glFinish),

    /* GLES extensions absent as link-time exports on the Mali userspace. */
    ASM2_IMPORT("glCompressedTexImage3DOES", asm2_glCompressedTexImage3DOES),
    ASM2_IMPORT("glCompressedTexSubImage3DOES",
                asm2_glCompressedTexSubImage3DOES),
    ASM2_IMPORT("glTexImage3DOES", asm2_glTexImage3DOES),
    ASM2_IMPORT("glTexSubImage3DOES", asm2_glTexSubImage3DOES),
    ASM2_IMPORT("glDeleteFencesNV", asm2_glDeleteFencesNV),
    ASM2_IMPORT("glGenFencesNV", asm2_glGenFencesNV),
    ASM2_IMPORT("glSetFenceNV", asm2_glSetFenceNV),
    ASM2_IMPORT("glFinishFenceNV", asm2_glFinishFenceNV),
    ASM2_IMPORT("glTestFenceNV", asm2_glTestFenceNV),

    /* NDK platform services.  Sensor stubs intentionally expose no sensor. */
    ASM2_IMPORT("ALooper_forThread", asm2_ALooper_forThread),
    ASM2_IMPORT("ALooper_prepare", asm2_ALooper_prepare),
    ASM2_IMPORT("ASensorManager_getInstance", asm2_ASensorManager_getInstance),
    ASM2_IMPORT("ASensorManager_getDefaultSensor",
                asm2_ASensorManager_getDefaultSensor),
    ASM2_IMPORT("ASensorManager_createEventQueue",
                asm2_ASensorManager_createEventQueue),
    ASM2_IMPORT("ASensorEventQueue_disableSensor",
                asm2_ASensorEventQueue_disableSensor),
    ASM2_IMPORT("ASensorEventQueue_enableSensor",
                asm2_ASensorEventQueue_enableSensor),
    ASM2_IMPORT("ASensorEventQueue_getEvents",
                asm2_ASensorEventQueue_getEvents),
    ASM2_IMPORT("ASensorEventQueue_setEventRate",
                asm2_ASensorEventQueue_setEventRate),

    /* OpenSL symbols are pointer-valued objects, backed by the SDL2 PCM
     * buffer-queue bridge. */
    ASM2_IMPORT("SL_IID_ENGINE", &asm2_sl_iid_engine),
    ASM2_IMPORT("SL_IID_BUFFERQUEUE", &asm2_sl_iid_bufferqueue),
    ASM2_IMPORT("SL_IID_PLAY", &asm2_sl_iid_play),
    ASM2_IMPORT("slCreateEngine", asm2_slCreateEngine),
};

size_t dynlib_numfunctions =
    sizeof(dynlib_functions) / sizeof(dynlib_functions[0]);

#if defined(__i386__)
void asm2_imports_initialize(void) {
  for (size_t index = 0; index < dynlib_numfunctions; ++index) {
    if (dynlib_functions[index].func != 0 ||
        strncmp(dynlib_functions[index].symbol, "gl", 2) != 0)
      continue;
    dynlib_functions[index].func =
        asm2_x86_gl_lazy_stub(dynlib_functions[index].symbol);
    if (!dynlib_functions[index].func) {
      fprintf(stderr, "ASM2_X86_GL_STUB_ERROR name=%s\n",
              dynlib_functions[index].symbol);
      abort();
    }
  }
}
#endif
