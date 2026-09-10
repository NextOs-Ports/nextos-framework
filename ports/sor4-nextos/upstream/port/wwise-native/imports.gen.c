// imports.gen.c — GERADO por new-port.sh para 'sor4wwise' (libWwise.so)
// 134 simbolos. Resolva os UNKNOWN no fim do arquivo.
#include "imports.h"
#include "so_util.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <math.h>
#include <ctype.h>
#include <wchar.h>
#include <wctype.h>
#include <time.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <errno.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <malloc.h>
#include <locale.h>
#include <signal.h>
#include <setjmp.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <pthread.h>
#include "opensles_shim.h"
#include "android_shim.h"
static int *bionic_errno(void){ return __errno_location(); }
#define __errno bionic_errno
extern void __stack_chk_fail(void);
extern unsigned long __stack_chk_guard;
// SL_IID_ANDROIDCONFIGURATION nao existe no shim -> dummy (config de stream Android = no-op)
static const long sl_IID_ANDROIDCONFIGURATION_dummy = 0;
// extern decls dos _fake + __cxa (def em pthread_fake.c/glibc)
extern int __cxa_atexit(void(*)(void*),void*,void*);
extern void __cxa_finalize(void*);
extern long pthread_attr_destroy_fake();
extern long pthread_attr_init_fake();
extern long pthread_attr_setdetachstate_fake();
extern long pthread_attr_setstacksize_fake();
extern long pthread_condattr_destroy_fake();
extern long pthread_condattr_init_fake();
extern long pthread_cond_broadcast_fake();
extern long pthread_cond_destroy_fake();
extern long pthread_cond_init_fake();
extern long pthread_cond_signal_fake();
extern long pthread_cond_wait_fake();
extern long pthread_create_fake();
extern long pthread_getspecific_fake();
extern long pthread_join_fake();
extern long pthread_key_create_fake();
extern long pthread_key_delete_fake();
extern long pthread_mutexattr_destroy_fake();
extern long pthread_mutexattr_init_fake();
extern long pthread_mutexattr_settype_fake();
extern long pthread_mutex_destroy_fake();
extern long pthread_mutex_init_fake();
extern long pthread_mutex_lock_fake();
extern long pthread_mutex_unlock_fake();
extern long pthread_once_fake();
extern long pthread_rwlock_rdlock_fake();
extern long pthread_rwlock_unlock_fake();
extern long pthread_rwlock_wrlock_fake();
extern long pthread_self_fake();
extern long pthread_setschedparam_fake();
extern long pthread_setspecific_fake();
extern long sem_destroy_fake();
extern long sem_init_fake();
extern long sem_post_fake();
extern long sem_wait_fake();

/* sigaction ABI bionic->glibc (bionic_shims.c): rotear o sigaction do Wwise pela nossa shim
 * conserta o ABI (32B bionic vs 152B glibc; oldact estourava a stack do caller) E permite o
 * gate CUP_NOSIGH impedir o Wwise de clobberar o handler de SIGSEGV do .NET CoreCLR. */
extern int my_sigaction(int sig, const void *act, void *oldact);

// === passthrough/pthread/shim: ligados automaticamente ===
DynLibFunction dynlib_functions[] = {
  {"sigaction", (uintptr_t)&my_sigaction},  // ABI bionic->glibc + gate CUP_NOSIGH (fix crash do save)
  {"abort", (uintptr_t)&abort},  // pass
  // TODO {"acosf", (uintptr_t)&stub_acosf},  // <<< IMPLEMENTAR
  // TODO {"android_set_abort_message", (uintptr_t)&stub_android_set_abort_message},  // <<< IMPLEMENTAR
  // TODO {"asinf", (uintptr_t)&stub_asinf},  // <<< IMPLEMENTAR
  {"atan2f", (uintptr_t)&atan2f},  // pass
  {"atoi", (uintptr_t)&atoi},  // pass
  {"clock_gettime", (uintptr_t)&clock_gettime},  // pass
  // TODO {"closelog", (uintptr_t)&stub_closelog},  // <<< IMPLEMENTAR
  {"cos", (uintptr_t)&cos},  // pass
  {"cosf", (uintptr_t)&cosf},  // pass
  {"__cxa_atexit", (uintptr_t)&__cxa_atexit},  // cxx
  {"__cxa_finalize", (uintptr_t)&__cxa_finalize},  // cxx
  // TODO {"dlclose", (uintptr_t)&stub_dlclose},  // <<< IMPLEMENTAR
  // TODO {"dlerror", (uintptr_t)&stub_dlerror},  // <<< IMPLEMENTAR
  // TODO {"dl_iterate_phdr", (uintptr_t)&stub_dl_iterate_phdr},  // <<< IMPLEMENTAR
  // TODO {"dlopen", (uintptr_t)&stub_dlopen},  // <<< IMPLEMENTAR
  // TODO {"dlsym", (uintptr_t)&stub_dlsym},  // <<< IMPLEMENTAR
  {"__errno", (uintptr_t)&__errno},  // pass
  {"exp", (uintptr_t)&exp},  // pass
  {"exp2", (uintptr_t)&exp2},  // pass
  // TODO {"exp2f", (uintptr_t)&stub_exp2f},  // <<< IMPLEMENTAR
  {"expf", (uintptr_t)&expf},  // pass
  {"fclose", (uintptr_t)&fclose},  // pass
  // TODO {"feof", (uintptr_t)&stub_feof},  // <<< IMPLEMENTAR
  {"fflush", (uintptr_t)&fflush},  // pass
  // TODO {"flockfile", (uintptr_t)&stub_flockfile},  // <<< IMPLEMENTAR
  {"fmodf", (uintptr_t)&fmodf},  // pass
  {"fopen", (uintptr_t)&fopen},  // pass
  {"fprintf", (uintptr_t)&fprintf},  // pass
  {"fputc", (uintptr_t)&fputc},  // pass
  {"fread", (uintptr_t)&fread},  // pass
  {"free", (uintptr_t)&free},  // pass
  // TODO {"freopen", (uintptr_t)&stub_freopen},  // <<< IMPLEMENTAR
  {"fseek", (uintptr_t)&fseek},  // pass
  // TODO {"fseeko", (uintptr_t)&stub_fseeko},  // <<< IMPLEMENTAR
  // TODO {"ftello", (uintptr_t)&stub_ftello},  // <<< IMPLEMENTAR
  // TODO {"funlockfile", (uintptr_t)&stub_funlockfile},  // <<< IMPLEMENTAR
  {"fwrite", (uintptr_t)&fwrite},  // pass
  // TODO {"getauxval", (uintptr_t)&stub_getauxval},  // <<< IMPLEMENTAR
  {"log", (uintptr_t)&log},  // pass
  {"log10", (uintptr_t)&log10},  // pass
  // TODO {"log10f", (uintptr_t)&stub_log10f},  // <<< IMPLEMENTAR
  {"malloc", (uintptr_t)&malloc},  // pass
  {"memchr", (uintptr_t)&memchr},  // pass
  {"memcmp", (uintptr_t)&memcmp},  // pass
  {"memcpy", (uintptr_t)&memcpy},  // pass
  // TODO {"__memcpy_chk", (uintptr_t)&stub___memcpy_chk},  // <<< IMPLEMENTAR
  {"memmove", (uintptr_t)&memmove},  // pass
  // TODO {"__memmove_chk", (uintptr_t)&stub___memmove_chk},  // <<< IMPLEMENTAR
  {"memset", (uintptr_t)&memset},  // pass
  {"mmap", (uintptr_t)&mmap},  // pass
  {"munmap", (uintptr_t)&munmap},  // pass
  // TODO {"openlog", (uintptr_t)&stub_openlog},  // <<< IMPLEMENTAR
  {"posix_memalign", (uintptr_t)&posix_memalign},  // pass
  {"pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy_fake},  // pthread wrapper (core)
  {"pthread_attr_init", (uintptr_t)&pthread_attr_init_fake},  // pthread wrapper (core)
  {"pthread_attr_setdetachstate", (uintptr_t)&pthread_attr_setdetachstate_fake},  // pthread wrapper (core)
  {"pthread_attr_setstacksize", (uintptr_t)&pthread_attr_setstacksize_fake},  // pthread wrapper (core)
  {"pthread_condattr_destroy", (uintptr_t)&pthread_condattr_destroy_fake},  // pthread wrapper (core)
  {"pthread_condattr_init", (uintptr_t)&pthread_condattr_init_fake},  // pthread wrapper (core)
  {"pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake},  // pthread wrapper (core)
  {"pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake},  // pthread wrapper (core)
  {"pthread_cond_init", (uintptr_t)&pthread_cond_init_fake},  // pthread wrapper (core)
  {"pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake},  // pthread wrapper (core)
  {"pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake},  // pthread wrapper (core)
  {"pthread_create", (uintptr_t)&pthread_create_fake},  // pthread wrapper (core)
  {"pthread_getspecific", (uintptr_t)&pthread_getspecific_fake},  // pthread wrapper (core)
  {"pthread_join", (uintptr_t)&pthread_join_fake},  // pthread wrapper (core)
  {"pthread_key_create", (uintptr_t)&pthread_key_create_fake},  // pthread wrapper (core)
  {"pthread_key_delete", (uintptr_t)&pthread_key_delete_fake},  // pthread wrapper (core)
  {"pthread_mutexattr_destroy", (uintptr_t)&pthread_mutexattr_destroy_fake},  // pthread wrapper (core)
  {"pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_fake},  // pthread wrapper (core)
  {"pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_fake},  // pthread wrapper (core)
  {"pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake},  // pthread wrapper (core)
  {"pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake},  // pthread wrapper (core)
  {"pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake},  // pthread wrapper (core)
  {"pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake},  // pthread wrapper (core)
  {"pthread_once", (uintptr_t)&pthread_once_fake},  // pthread wrapper (core)
  {"pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock_fake},  // pthread wrapper (core)
  {"pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock_fake},  // pthread wrapper (core)
  {"pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock_fake},  // pthread wrapper (core)
  {"pthread_self", (uintptr_t)&pthread_self_fake},  // pthread wrapper (core)
  {"pthread_setschedparam", (uintptr_t)&pthread_setschedparam_fake},  // pthread wrapper (core)
  {"pthread_setspecific", (uintptr_t)&pthread_setspecific_fake},  // pthread wrapper (core)
  {"qsort", (uintptr_t)&qsort},  // pass
  {"rand", (uintptr_t)&rand},  // pass
  {"realloc", (uintptr_t)&realloc},  // pass
  // TODO {"sched_get_priority_max", (uintptr_t)&stub_sched_get_priority_max},  // <<< IMPLEMENTAR
  // TODO {"sched_get_priority_min", (uintptr_t)&stub_sched_get_priority_min},  // <<< IMPLEMENTAR
  {"sem_destroy", (uintptr_t)&sem_destroy_fake},  // pthread wrapper (core)
  {"sem_init", (uintptr_t)&sem_init_fake},  // pthread wrapper (core)
  {"sem_post", (uintptr_t)&sem_post_fake},  // pthread wrapper (core)
  {"sem_wait", (uintptr_t)&sem_wait_fake},  // pthread wrapper (core)
  // TODO {"__sF", (uintptr_t)&stub___sF},  // <<< IMPLEMENTAR
  // TODO {"sincos", (uintptr_t)&stub_sincos},  // <<< IMPLEMENTAR
  // TODO {"sincosf", (uintptr_t)&stub_sincosf},  // <<< IMPLEMENTAR
  {"sinf", (uintptr_t)&sinf},  // pass
  {"slCreateEngine", (uintptr_t)&slCreateEngine_shim},  // opensles_shim
  {"SL_IID_ANDROIDCONFIGURATION", (uintptr_t)&sl_IID_ANDROIDCONFIGURATION_dummy},  // dummy (android config = no-op)
  {"SL_IID_BUFFERQUEUE", (uintptr_t)&sl_IID_BUFFERQUEUE},  // opensles_shim
  {"SL_IID_ENGINE", (uintptr_t)&sl_IID_ENGINE},  // opensles_shim
  {"SL_IID_PLAY", (uintptr_t)&sl_IID_PLAY},  // opensles_shim
  {"__stack_chk_fail", (uintptr_t)&__stack_chk_fail},  // abi
  {"stat", (uintptr_t)&stat},  // pass
  {"strcmp", (uintptr_t)&strcmp},  // pass
  {"strcpy", (uintptr_t)&strcpy},  // pass
  {"strlen", (uintptr_t)&strlen},  // pass
  // TODO {"__strlen_chk", (uintptr_t)&stub___strlen_chk},  // <<< IMPLEMENTAR
  {"strncat", (uintptr_t)&strncat},  // pass
  // TODO {"__strncat_chk", (uintptr_t)&stub___strncat_chk},  // <<< IMPLEMENTAR
  {"strncmp", (uintptr_t)&strncmp},  // pass
  {"strncpy", (uintptr_t)&strncpy},  // pass
  // TODO {"__strncpy_chk", (uintptr_t)&stub___strncpy_chk},  // <<< IMPLEMENTAR
  // TODO {"__strncpy_chk2", (uintptr_t)&stub___strncpy_chk2},  // <<< IMPLEMENTAR
  {"strtol", (uintptr_t)&strtol},  // pass
  // TODO {"syscall", (uintptr_t)&stub_syscall},  // <<< IMPLEMENTAR
  {"sysconf", (uintptr_t)&sysconf},  // pass
  // TODO {"syslog", (uintptr_t)&stub_syslog},  // <<< IMPLEMENTAR
  // TODO {"__system_property_get", (uintptr_t)&stub___system_property_get},  // <<< IMPLEMENTAR
  {"tanf", (uintptr_t)&tanf},  // pass
  {"time", (uintptr_t)&time},  // pass
  {"usleep", (uintptr_t)&usleep},  // pass
  // TODO {"vasprintf", (uintptr_t)&stub_vasprintf},  // <<< IMPLEMENTAR
  // TODO {"vfprintf", (uintptr_t)&stub_vfprintf},  // <<< IMPLEMENTAR
  {"vsnprintf", (uintptr_t)&vsnprintf},  // pass
  // TODO {"__vsnprintf_chk", (uintptr_t)&stub___vsnprintf_chk},  // <<< IMPLEMENTAR
};
const int dynlib_functions_count = sizeof(dynlib_functions)/sizeof(dynlib_functions[0]);
size_t dynlib_numfunctions = sizeof(dynlib_functions)/sizeof(dynlib_functions[0]);

// ===================== SIMBOLOS A IMPLEMENTAR =====================
//   acosf
//   android_set_abort_message
//   asinf
//   closelog
//   dlclose
//   dlerror
//   dl_iterate_phdr
//   dlopen
//   dlsym
//   exp2f
//   feof
//   flockfile
//   freopen
//   fseeko
//   ftello
//   funlockfile
//   getauxval
//   log10f
//   __memcpy_chk
//   __memmove_chk
//   openlog
//   sched_get_priority_max
//   sched_get_priority_min
//   __sF
//   sincos
//   sincosf
//   __strlen_chk
//   __strncat_chk
//   __strncpy_chk
//   __strncpy_chk2
//   syscall
//   syslog
//   __system_property_get
//   vasprintf
//   vfprintf
//   __vsnprintf_chk
