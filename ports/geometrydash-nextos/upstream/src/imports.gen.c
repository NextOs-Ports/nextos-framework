// imports.gen.c — GERADO por new-port.sh para 'gdsubzero' (libcocos2dcpp.so)
// 458 simbolos. Resolva os UNKNOWN no fim do arquivo.
#include "imports.h"
#include "so_util.h"
#include <stdio.h>

// === passthrough/pthread/shim: ligados automaticamente ===
DynLibFunction dynlib_functions[] = {
  {"abort", (uintptr_t)&abort},  // pass
  {"abs", (uintptr_t)&abs},  // pass
  // TODO {"accept", (uintptr_t)&stub_accept},  // <<< IMPLEMENTAR
  {"access", (uintptr_t)&access},  // pass
  {"acos", (uintptr_t)&acos},  // pass
  // TODO {"acosf", (uintptr_t)&stub_acosf},  // <<< IMPLEMENTAR
  // TODO {"alarm", (uintptr_t)&stub_alarm},  // <<< IMPLEMENTAR
  {"__android_log_print", (uintptr_t)&__android_log_print},  // liblog
  // TODO {"android_set_abort_message", (uintptr_t)&stub_android_set_abort_message},  // <<< IMPLEMENTAR
  // TODO {"asinf", (uintptr_t)&stub_asinf},  // <<< IMPLEMENTAR
  // TODO {"asinhf", (uintptr_t)&stub_asinhf},  // <<< IMPLEMENTAR
  {"atan2", (uintptr_t)&atan2},  // pass
  {"atan2f", (uintptr_t)&atan2f},  // pass
  // TODO {"atanf", (uintptr_t)&stub_atanf},  // <<< IMPLEMENTAR
  {"atof", (uintptr_t)&atof},  // pass
  {"atoi", (uintptr_t)&atoi},  // pass
  // TODO {"basename", (uintptr_t)&stub_basename},  // <<< IMPLEMENTAR
  // TODO {"bind", (uintptr_t)&stub_bind},  // <<< IMPLEMENTAR
  {"bsearch", (uintptr_t)&bsearch},  // pass
  // TODO {"btowc", (uintptr_t)&stub_btowc},  // <<< IMPLEMENTAR
  {"calloc", (uintptr_t)&calloc},  // pass
  // TODO {"chmod", (uintptr_t)&stub_chmod},  // <<< IMPLEMENTAR
  // TODO {"clock", (uintptr_t)&stub_clock},  // <<< IMPLEMENTAR
  {"clock_gettime", (uintptr_t)&clock_gettime},  // pass
  {"close", (uintptr_t)&close},  // pass
  {"closedir", (uintptr_t)&closedir},  // pass
  // TODO {"closelog", (uintptr_t)&stub_closelog},  // <<< IMPLEMENTAR
  // TODO {"connect", (uintptr_t)&stub_connect},  // <<< IMPLEMENTAR
  {"cos", (uintptr_t)&cos},  // pass
  {"cosf", (uintptr_t)&cosf},  // pass
  // TODO {"__ctype_get_mb_cur_max", (uintptr_t)&stub___ctype_get_mb_cur_max},  // <<< IMPLEMENTAR
  {"__cxa_atexit", (uintptr_t)&__cxa_atexit},  // cxx
  {"__cxa_finalize", (uintptr_t)&__cxa_finalize},  // cxx
  {"__cxa_thread_atexit_impl", (uintptr_t)&__cxa_thread_atexit_impl},  // cxx
  // TODO {"difftime", (uintptr_t)&stub_difftime},  // <<< IMPLEMENTAR
  // TODO {"dlclose", (uintptr_t)&stub_dlclose},  // <<< IMPLEMENTAR
  // TODO {"dlerror", (uintptr_t)&stub_dlerror},  // <<< IMPLEMENTAR
  // TODO {"dl_iterate_phdr", (uintptr_t)&stub_dl_iterate_phdr},  // <<< IMPLEMENTAR
  // TODO {"dlopen", (uintptr_t)&stub_dlopen},  // <<< IMPLEMENTAR
  // TODO {"dlsym", (uintptr_t)&stub_dlsym},  // <<< IMPLEMENTAR
  {"dup2", (uintptr_t)&dup2},  // pass
  {"__errno", (uintptr_t)&__errno},  // pass
  // TODO {"execl", (uintptr_t)&stub_execl},  // <<< IMPLEMENTAR
  {"exit", (uintptr_t)&exit},  // pass
  {"exp", (uintptr_t)&exp},  // pass
  {"expf", (uintptr_t)&expf},  // pass
  {"fclose", (uintptr_t)&fclose},  // pass
  {"fcntl", (uintptr_t)&fcntl},  // pass
  {"fdopen", (uintptr_t)&fdopen},  // pass
  // TODO {"feof", (uintptr_t)&stub_feof},  // <<< IMPLEMENTAR
  // TODO {"ferror", (uintptr_t)&stub_ferror},  // <<< IMPLEMENTAR
  {"fflush", (uintptr_t)&fflush},  // pass
  {"fgets", (uintptr_t)&fgets},  // pass
  {"fileno", (uintptr_t)&fileno},  // pass
  {"fmax", (uintptr_t)&fmax},  // pass
  {"fmin", (uintptr_t)&fmin},  // pass
  {"fmod", (uintptr_t)&fmod},  // pass
  // TODO {"FMOD_Channel_GetDelay", (uintptr_t)&stub_FMOD_Channel_GetDelay},  // <<< IMPLEMENTAR
  // TODO {"FMOD_Channel_GetDSPClock", (uintptr_t)&stub_FMOD_Channel_GetDSPClock},  // <<< IMPLEMENTAR
  // TODO {"FMOD_Channel_GetFadePoints", (uintptr_t)&stub_FMOD_Channel_GetFadePoints},  // <<< IMPLEMENTAR
  // TODO {"FMOD_Channel_RemoveFadePoints", (uintptr_t)&stub_FMOD_Channel_RemoveFadePoints},  // <<< IMPLEMENTAR
  // TODO {"FMOD_Debug_Initialize", (uintptr_t)&stub_FMOD_Debug_Initialize},  // <<< IMPLEMENTAR
  {"fmodf", (uintptr_t)&fmodf},  // pass
  // TODO {"FMOD_Memory_GetStats", (uintptr_t)&stub_FMOD_Memory_GetStats},  // <<< IMPLEMENTAR
  // TODO {"FMOD_System_Create", (uintptr_t)&stub_FMOD_System_Create},  // <<< IMPLEMENTAR
  // TODO {"FMOD_System_LockDSP", (uintptr_t)&stub_FMOD_System_LockDSP},  // <<< IMPLEMENTAR
  // TODO {"FMOD_System_UnlockDSP", (uintptr_t)&stub_FMOD_System_UnlockDSP},  // <<< IMPLEMENTAR
  {"fopen", (uintptr_t)&fopen},  // pass
  // TODO {"fork", (uintptr_t)&stub_fork},  // <<< IMPLEMENTAR
  {"fprintf", (uintptr_t)&fprintf},  // pass
  {"fputc", (uintptr_t)&fputc},  // pass
  {"fputs", (uintptr_t)&fputs},  // pass
  {"fread", (uintptr_t)&fread},  // pass
  {"free", (uintptr_t)&free},  // pass
  // TODO {"freeaddrinfo", (uintptr_t)&stub_freeaddrinfo},  // <<< IMPLEMENTAR
  // TODO {"freelocale", (uintptr_t)&stub_freelocale},  // <<< IMPLEMENTAR
  {"frexp", (uintptr_t)&frexp},  // pass
  {"fseek", (uintptr_t)&fseek},  // pass
  // TODO {"fseeko", (uintptr_t)&stub_fseeko},  // <<< IMPLEMENTAR
  {"fstat", (uintptr_t)&fstat},  // pass
  {"ftell", (uintptr_t)&ftell},  // pass
  // TODO {"ftello", (uintptr_t)&stub_ftello},  // <<< IMPLEMENTAR
  {"fwrite", (uintptr_t)&fwrite},  // pass
  // TODO {"gai_strerror", (uintptr_t)&stub_gai_strerror},  // <<< IMPLEMENTAR
  // TODO {"getaddrinfo", (uintptr_t)&stub_getaddrinfo},  // <<< IMPLEMENTAR
  // TODO {"getauxval", (uintptr_t)&stub_getauxval},  // <<< IMPLEMENTAR
  {"getc", (uintptr_t)&getc},  // pass
  // TODO {"getegid", (uintptr_t)&stub_getegid},  // <<< IMPLEMENTAR
  {"getenv", (uintptr_t)&getenv},  // pass
  // TODO {"geteuid", (uintptr_t)&stub_geteuid},  // <<< IMPLEMENTAR
  // TODO {"getgid", (uintptr_t)&stub_getgid},  // <<< IMPLEMENTAR
  // TODO {"gethostbyname", (uintptr_t)&stub_gethostbyname},  // <<< IMPLEMENTAR
  // TODO {"gethostname", (uintptr_t)&stub_gethostname},  // <<< IMPLEMENTAR
  // TODO {"getnameinfo", (uintptr_t)&stub_getnameinfo},  // <<< IMPLEMENTAR
  // TODO {"getpeername", (uintptr_t)&stub_getpeername},  // <<< IMPLEMENTAR
  {"getpid", (uintptr_t)&getpid},  // pass
  // TODO {"getpwuid", (uintptr_t)&stub_getpwuid},  // <<< IMPLEMENTAR
  // TODO {"getpwuid_r", (uintptr_t)&stub_getpwuid_r},  // <<< IMPLEMENTAR
  // TODO {"getsockname", (uintptr_t)&stub_getsockname},  // <<< IMPLEMENTAR
  // TODO {"getsockopt", (uintptr_t)&stub_getsockopt},  // <<< IMPLEMENTAR
  {"gettimeofday", (uintptr_t)&gettimeofday},  // pass
  // TODO {"getuid", (uintptr_t)&stub_getuid},  // <<< IMPLEMENTAR
  {"glActiveTexture", (uintptr_t)&glActiveTexture},  // gles
  {"glAttachShader", (uintptr_t)&glAttachShader},  // gles
  {"glBindAttribLocation", (uintptr_t)&glBindAttribLocation},  // gles
  {"glBindBuffer", (uintptr_t)&glBindBuffer},  // gles
  {"glBindFramebuffer", (uintptr_t)&glBindFramebuffer},  // gles
  {"glBindRenderbuffer", (uintptr_t)&glBindRenderbuffer},  // gles
  {"glBindTexture", (uintptr_t)&glBindTexture},  // gles
  {"glBlendEquation", (uintptr_t)&glBlendEquation},  // gles
  {"glBlendFunc", (uintptr_t)&glBlendFunc},  // gles
  {"glBufferData", (uintptr_t)&glBufferData},  // gles
  {"glBufferSubData", (uintptr_t)&glBufferSubData},  // gles
  {"glCheckFramebufferStatus", (uintptr_t)&glCheckFramebufferStatus},  // gles
  {"glClear", (uintptr_t)&glClear},  // gles
  {"glClearColor", (uintptr_t)&glClearColor},  // gles
  {"glClearDepthf", (uintptr_t)&glClearDepthf},  // gles
  {"glClearStencil", (uintptr_t)&glClearStencil},  // gles
  {"glCompileShader", (uintptr_t)&glCompileShader},  // gles
  {"glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D},  // gles
  {"glCreateProgram", (uintptr_t)&glCreateProgram},  // gles
  {"glCreateShader", (uintptr_t)&glCreateShader},  // gles
  {"glDeleteBuffers", (uintptr_t)&glDeleteBuffers},  // gles
  {"glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers},  // gles
  {"glDeleteProgram", (uintptr_t)&glDeleteProgram},  // gles
  {"glDeleteRenderbuffers", (uintptr_t)&glDeleteRenderbuffers},  // gles
  {"glDeleteShader", (uintptr_t)&glDeleteShader},  // gles
  {"glDeleteTextures", (uintptr_t)&glDeleteTextures},  // gles
  {"glDepthFunc", (uintptr_t)&glDepthFunc},  // gles
  {"glDepthMask", (uintptr_t)&glDepthMask},  // gles
  {"glDisable", (uintptr_t)&glDisable},  // gles
  {"glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray},  // gles
  {"glDrawArrays", (uintptr_t)&glDrawArrays},  // gles
  {"glDrawElements", (uintptr_t)&glDrawElements},  // gles
  {"glEnable", (uintptr_t)&glEnable},  // gles
  {"glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray},  // gles
  {"glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbuffer},  // gles
  {"glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D},  // gles
  {"glGenBuffers", (uintptr_t)&glGenBuffers},  // gles
  {"glGenerateMipmap", (uintptr_t)&glGenerateMipmap},  // gles
  {"glGenFramebuffers", (uintptr_t)&glGenFramebuffers},  // gles
  {"glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers},  // gles
  {"glGenTextures", (uintptr_t)&glGenTextures},  // gles
  {"glGetBooleanv", (uintptr_t)&glGetBooleanv},  // gles
  {"glGetError", (uintptr_t)&glGetError},  // gles
  {"glGetFloatv", (uintptr_t)&glGetFloatv},  // gles
  {"glGetIntegerv", (uintptr_t)&glGetIntegerv},  // gles
  {"glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog},  // gles
  {"glGetProgramiv", (uintptr_t)&glGetProgramiv},  // gles
  {"glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLog},  // gles
  {"glGetShaderiv", (uintptr_t)&glGetShaderiv},  // gles
  {"glGetShaderSource", (uintptr_t)&glGetShaderSource},  // gles
  {"glGetString", (uintptr_t)&glGetString},  // gles
  {"glGetUniformLocation", (uintptr_t)&glGetUniformLocation},  // gles
  {"glIsEnabled", (uintptr_t)&glIsEnabled},  // gles
  {"glLineWidth", (uintptr_t)&glLineWidth},  // gles
  {"glLinkProgram", (uintptr_t)&glLinkProgram},  // gles
  {"glPixelStorei", (uintptr_t)&glPixelStorei},  // gles
  {"glReadPixels", (uintptr_t)&glReadPixels},  // gles
  {"glRenderbufferStorage", (uintptr_t)&glRenderbufferStorage},  // gles
  {"glScissor", (uintptr_t)&glScissor},  // gles
  {"glShaderSource", (uintptr_t)&glShaderSource},  // gles
  {"glStencilFunc", (uintptr_t)&glStencilFunc},  // gles
  {"glStencilMask", (uintptr_t)&glStencilMask},  // gles
  {"glStencilOp", (uintptr_t)&glStencilOp},  // gles
  {"glTexImage2D", (uintptr_t)&glTexImage2D},  // gles
  {"glTexParameteri", (uintptr_t)&glTexParameteri},  // gles
  {"glUniform1f", (uintptr_t)&glUniform1f},  // gles
  {"glUniform1i", (uintptr_t)&glUniform1i},  // gles
  {"glUniform2f", (uintptr_t)&glUniform2f},  // gles
  {"glUniform2fv", (uintptr_t)&glUniform2fv},  // gles
  {"glUniform2i", (uintptr_t)&glUniform2i},  // gles
  {"glUniform2iv", (uintptr_t)&glUniform2iv},  // gles
  {"glUniform3f", (uintptr_t)&glUniform3f},  // gles
  {"glUniform3fv", (uintptr_t)&glUniform3fv},  // gles
  {"glUniform3i", (uintptr_t)&glUniform3i},  // gles
  {"glUniform3iv", (uintptr_t)&glUniform3iv},  // gles
  {"glUniform4f", (uintptr_t)&glUniform4f},  // gles
  {"glUniform4fv", (uintptr_t)&glUniform4fv},  // gles
  {"glUniform4i", (uintptr_t)&glUniform4i},  // gles
  {"glUniform4iv", (uintptr_t)&glUniform4iv},  // gles
  {"glUniformMatrix3fv", (uintptr_t)&glUniformMatrix3fv},  // gles
  {"glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv},  // gles
  {"glUseProgram", (uintptr_t)&glUseProgram},  // gles
  {"glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer},  // gles
  {"glViewport", (uintptr_t)&glViewport},  // gles
  {"gmtime", (uintptr_t)&gmtime},  // pass
  // TODO {"gmtime_r", (uintptr_t)&stub_gmtime_r},  // <<< IMPLEMENTAR
  // TODO {"if_nametoindex", (uintptr_t)&stub_if_nametoindex},  // <<< IMPLEMENTAR
  // TODO {"inet_ntop", (uintptr_t)&stub_inet_ntop},  // <<< IMPLEMENTAR
  // TODO {"inet_pton", (uintptr_t)&stub_inet_pton},  // <<< IMPLEMENTAR
  // TODO {"initgroups", (uintptr_t)&stub_initgroups},  // <<< IMPLEMENTAR
  {"ioctl", (uintptr_t)&ioctl},  // pass
  {"isalnum", (uintptr_t)&isalnum},  // pass
  {"isalpha", (uintptr_t)&isalpha},  // pass
  // TODO {"isascii", (uintptr_t)&stub_isascii},  // <<< IMPLEMENTAR
  // TODO {"isdigit_l", (uintptr_t)&stub_isdigit_l},  // <<< IMPLEMENTAR
  // TODO {"isgraph", (uintptr_t)&stub_isgraph},  // <<< IMPLEMENTAR
  {"islower", (uintptr_t)&islower},  // pass
  // TODO {"islower_l", (uintptr_t)&stub_islower_l},  // <<< IMPLEMENTAR
  // TODO {"__isnanf", (uintptr_t)&stub___isnanf},  // <<< IMPLEMENTAR
  // TODO {"isprint", (uintptr_t)&stub_isprint},  // <<< IMPLEMENTAR
  {"isspace", (uintptr_t)&isspace},  // pass
  {"isupper", (uintptr_t)&isupper},  // pass
  // TODO {"isupper_l", (uintptr_t)&stub_isupper_l},  // <<< IMPLEMENTAR
  // TODO {"iswalpha_l", (uintptr_t)&stub_iswalpha_l},  // <<< IMPLEMENTAR
  // TODO {"iswblank_l", (uintptr_t)&stub_iswblank_l},  // <<< IMPLEMENTAR
  // TODO {"iswcntrl_l", (uintptr_t)&stub_iswcntrl_l},  // <<< IMPLEMENTAR
  // TODO {"iswdigit_l", (uintptr_t)&stub_iswdigit_l},  // <<< IMPLEMENTAR
  // TODO {"iswlower_l", (uintptr_t)&stub_iswlower_l},  // <<< IMPLEMENTAR
  // TODO {"iswprint_l", (uintptr_t)&stub_iswprint_l},  // <<< IMPLEMENTAR
  // TODO {"iswpunct_l", (uintptr_t)&stub_iswpunct_l},  // <<< IMPLEMENTAR
  // TODO {"iswspace_l", (uintptr_t)&stub_iswspace_l},  // <<< IMPLEMENTAR
  // TODO {"iswupper_l", (uintptr_t)&stub_iswupper_l},  // <<< IMPLEMENTAR
  // TODO {"iswxdigit_l", (uintptr_t)&stub_iswxdigit_l},  // <<< IMPLEMENTAR
  // TODO {"isxdigit", (uintptr_t)&stub_isxdigit},  // <<< IMPLEMENTAR
  // TODO {"isxdigit_l", (uintptr_t)&stub_isxdigit_l},  // <<< IMPLEMENTAR
  // TODO {"kill", (uintptr_t)&stub_kill},  // <<< IMPLEMENTAR
  // TODO {"listen", (uintptr_t)&stub_listen},  // <<< IMPLEMENTAR
  {"localeconv", (uintptr_t)&localeconv},  // pass
  {"log", (uintptr_t)&log},  // pass
  {"log10", (uintptr_t)&log10},  // pass
  {"logf", (uintptr_t)&logf},  // pass
  // TODO {"longjmp", (uintptr_t)&stub_longjmp},  // <<< IMPLEMENTAR
  // TODO {"lround", (uintptr_t)&stub_lround},  // <<< IMPLEMENTAR
  {"lseek", (uintptr_t)&lseek},  // pass
  {"madvise", (uintptr_t)&madvise},  // pass
  {"malloc", (uintptr_t)&malloc},  // pass
  // TODO {"mbrlen", (uintptr_t)&stub_mbrlen},  // <<< IMPLEMENTAR
  // TODO {"mbrtowc", (uintptr_t)&stub_mbrtowc},  // <<< IMPLEMENTAR
  // TODO {"mbsnrtowcs", (uintptr_t)&stub_mbsnrtowcs},  // <<< IMPLEMENTAR
  // TODO {"mbsrtowcs", (uintptr_t)&stub_mbsrtowcs},  // <<< IMPLEMENTAR
  // TODO {"mbtowc", (uintptr_t)&stub_mbtowc},  // <<< IMPLEMENTAR
  {"memchr", (uintptr_t)&memchr},  // pass
  {"memcmp", (uintptr_t)&memcmp},  // pass
  {"memcpy", (uintptr_t)&memcpy},  // pass
  {"memmove", (uintptr_t)&memmove},  // pass
  // TODO {"memrchr", (uintptr_t)&stub_memrchr},  // <<< IMPLEMENTAR
  {"memset", (uintptr_t)&memset},  // pass
  {"mkdir", (uintptr_t)&mkdir},  // pass
  // TODO {"mlock", (uintptr_t)&stub_mlock},  // <<< IMPLEMENTAR
  {"mmap", (uintptr_t)&mmap},  // pass
  {"modf", (uintptr_t)&modf},  // pass
  {"mprotect", (uintptr_t)&mprotect},  // pass
  {"munmap", (uintptr_t)&munmap},  // pass
  {"nanosleep", (uintptr_t)&nanosleep},  // pass
  // TODO {"newlocale", (uintptr_t)&stub_newlocale},  // <<< IMPLEMENTAR
  {"open", (uintptr_t)&open},  // pass
  {"opendir", (uintptr_t)&opendir},  // pass
  // TODO {"openlog", (uintptr_t)&stub_openlog},  // <<< IMPLEMENTAR
  {"pipe", (uintptr_t)&pipe},  // pass
  // TODO {"poll", (uintptr_t)&stub_poll},  // <<< IMPLEMENTAR
  {"pow", (uintptr_t)&pow},  // pass
  {"powf", (uintptr_t)&powf},  // pass
  {"printf", (uintptr_t)&printf},  // pass
  {"pthread_attr_init", (uintptr_t)&pthread_attr_init_fake},  // pthread wrapper (core)
  {"pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake},  // pthread wrapper (core)
  {"pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake},  // pthread wrapper (core)
  {"pthread_cond_init", (uintptr_t)&pthread_cond_init_fake},  // pthread wrapper (core)
  {"pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake},  // pthread wrapper (core)
  {"pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_fake},  // pthread wrapper (core)
  {"pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake},  // pthread wrapper (core)
  {"pthread_create", (uintptr_t)&pthread_create_fake},  // pthread wrapper (core)
  {"pthread_detach", (uintptr_t)&pthread_detach_fake},  // pthread wrapper (core)
  {"pthread_equal", (uintptr_t)&pthread_equal_fake},  // pthread wrapper (core)
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
  {"pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake},  // pthread wrapper (core)
  {"pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake},  // pthread wrapper (core)
  {"pthread_once", (uintptr_t)&pthread_once_fake},  // pthread wrapper (core)
  {"pthread_rwlock_destroy", (uintptr_t)&pthread_rwlock_destroy_fake},  // pthread wrapper (core)
  {"pthread_rwlock_init", (uintptr_t)&pthread_rwlock_init_fake},  // pthread wrapper (core)
  {"pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock_fake},  // pthread wrapper (core)
  {"pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock_fake},  // pthread wrapper (core)
  {"pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock_fake},  // pthread wrapper (core)
  {"pthread_self", (uintptr_t)&pthread_self_fake},  // pthread wrapper (core)
  {"pthread_setspecific", (uintptr_t)&pthread_setspecific_fake},  // pthread wrapper (core)
  {"qsort", (uintptr_t)&qsort},  // pass
  {"rand", (uintptr_t)&rand},  // pass
  {"read", (uintptr_t)&read},  // pass
  {"readdir", (uintptr_t)&readdir},  // pass
  {"realloc", (uintptr_t)&realloc},  // pass
  // TODO {"recv", (uintptr_t)&stub_recv},  // <<< IMPLEMENTAR
  // TODO {"recvfrom", (uintptr_t)&stub_recvfrom},  // <<< IMPLEMENTAR
  // TODO {"remove", (uintptr_t)&stub_remove},  // <<< IMPLEMENTAR
  {"rename", (uintptr_t)&rename},  // pass
  {"sched_yield", (uintptr_t)&sched_yield},  // pass
  // TODO {"send", (uintptr_t)&stub_send},  // <<< IMPLEMENTAR
  // TODO {"sendto", (uintptr_t)&stub_sendto},  // <<< IMPLEMENTAR
  // TODO {"setbuf", (uintptr_t)&stub_setbuf},  // <<< IMPLEMENTAR
  // TODO {"setgid", (uintptr_t)&stub_setgid},  // <<< IMPLEMENTAR
  // TODO {"setjmp", (uintptr_t)&stub_setjmp},  // <<< IMPLEMENTAR
  {"setlocale", (uintptr_t)&setlocale},  // pass
  // TODO {"setsockopt", (uintptr_t)&stub_setsockopt},  // <<< IMPLEMENTAR
  // TODO {"setuid", (uintptr_t)&stub_setuid},  // <<< IMPLEMENTAR
  // TODO {"__sF", (uintptr_t)&stub___sF},  // <<< IMPLEMENTAR
  // TODO {"shutdown", (uintptr_t)&stub_shutdown},  // <<< IMPLEMENTAR
  // TODO {"sigaction", (uintptr_t)&stub_sigaction},  // <<< IMPLEMENTAR
  // TODO {"sigaddset", (uintptr_t)&stub_sigaddset},  // <<< IMPLEMENTAR
  // TODO {"sigdelset", (uintptr_t)&stub_sigdelset},  // <<< IMPLEMENTAR
  // TODO {"sigemptyset", (uintptr_t)&stub_sigemptyset},  // <<< IMPLEMENTAR
  // TODO {"sigfillset", (uintptr_t)&stub_sigfillset},  // <<< IMPLEMENTAR
  // TODO {"siglongjmp", (uintptr_t)&stub_siglongjmp},  // <<< IMPLEMENTAR
  // TODO {"signal", (uintptr_t)&stub_signal},  // <<< IMPLEMENTAR
  // TODO {"__signbit", (uintptr_t)&stub___signbit},  // <<< IMPLEMENTAR
  // TODO {"sigprocmask", (uintptr_t)&stub_sigprocmask},  // <<< IMPLEMENTAR
  // TODO {"sigsetjmp", (uintptr_t)&stub_sigsetjmp},  // <<< IMPLEMENTAR
  {"sinf", (uintptr_t)&sinf},  // pass
  {"snprintf", (uintptr_t)&snprintf},  // pass
  // TODO {"socket", (uintptr_t)&stub_socket},  // <<< IMPLEMENTAR
  // TODO {"socketpair", (uintptr_t)&stub_socketpair},  // <<< IMPLEMENTAR
  {"sprintf", (uintptr_t)&sprintf},  // pass
  {"sqrt", (uintptr_t)&sqrt},  // pass
  {"sqrtf", (uintptr_t)&sqrtf},  // pass
  {"srand", (uintptr_t)&srand},  // pass
  {"sscanf", (uintptr_t)&sscanf},  // pass
  {"__stack_chk_fail", (uintptr_t)&__stack_chk_fail},  // abi
  {"__stack_chk_guard", (uintptr_t)&__stack_chk_guard},  // abi
  {"stat", (uintptr_t)&stat},  // pass
  {"strcasecmp", (uintptr_t)&strcasecmp},  // pass
  {"strcat", (uintptr_t)&strcat},  // pass
  {"strchr", (uintptr_t)&strchr},  // pass
  {"strcmp", (uintptr_t)&strcmp},  // pass
  // TODO {"strcoll_l", (uintptr_t)&stub_strcoll_l},  // <<< IMPLEMENTAR
  {"strcpy", (uintptr_t)&strcpy},  // pass
  // TODO {"strcspn", (uintptr_t)&stub_strcspn},  // <<< IMPLEMENTAR
  {"strdup", (uintptr_t)&strdup},  // pass
  {"strerror", (uintptr_t)&strerror},  // pass
  // TODO {"strerror_r", (uintptr_t)&stub_strerror_r},  // <<< IMPLEMENTAR
  // TODO {"strftime_l", (uintptr_t)&stub_strftime_l},  // <<< IMPLEMENTAR
  {"strlen", (uintptr_t)&strlen},  // pass
  {"strncasecmp", (uintptr_t)&strncasecmp},  // pass
  {"strncmp", (uintptr_t)&strncmp},  // pass
  {"strncpy", (uintptr_t)&strncpy},  // pass
  // TODO {"strpbrk", (uintptr_t)&stub_strpbrk},  // <<< IMPLEMENTAR
  {"strrchr", (uintptr_t)&strrchr},  // pass
  // TODO {"strspn", (uintptr_t)&stub_strspn},  // <<< IMPLEMENTAR
  {"strstr", (uintptr_t)&strstr},  // pass
  {"strtod", (uintptr_t)&strtod},  // pass
  {"strtof", (uintptr_t)&strtof},  // pass
  {"strtok", (uintptr_t)&strtok},  // pass
  // TODO {"strtok_r", (uintptr_t)&stub_strtok_r},  // <<< IMPLEMENTAR
  {"strtol", (uintptr_t)&strtol},  // pass
  // TODO {"strtold", (uintptr_t)&stub_strtold},  // <<< IMPLEMENTAR
  // TODO {"strtold_l", (uintptr_t)&stub_strtold_l},  // <<< IMPLEMENTAR
  // TODO {"strtoll", (uintptr_t)&stub_strtoll},  // <<< IMPLEMENTAR
  // TODO {"strtoll_l", (uintptr_t)&stub_strtoll_l},  // <<< IMPLEMENTAR
  {"strtoul", (uintptr_t)&strtoul},  // pass
  // TODO {"strtoull", (uintptr_t)&stub_strtoull},  // <<< IMPLEMENTAR
  // TODO {"strtoull_l", (uintptr_t)&stub_strtoull_l},  // <<< IMPLEMENTAR
  // TODO {"strxfrm_l", (uintptr_t)&stub_strxfrm_l},  // <<< IMPLEMENTAR
  // TODO {"swprintf", (uintptr_t)&stub_swprintf},  // <<< IMPLEMENTAR
  {"sysconf", (uintptr_t)&sysconf},  // pass
  // TODO {"syslog", (uintptr_t)&stub_syslog},  // <<< IMPLEMENTAR
  {"tanf", (uintptr_t)&tanf},  // pass
  // TODO {"tanhf", (uintptr_t)&stub_tanhf},  // <<< IMPLEMENTAR
  // TODO {"tcgetattr", (uintptr_t)&stub_tcgetattr},  // <<< IMPLEMENTAR
  // TODO {"tcsetattr", (uintptr_t)&stub_tcsetattr},  // <<< IMPLEMENTAR
  {"time", (uintptr_t)&time},  // pass
  {"tolower", (uintptr_t)&tolower},  // pass
  // TODO {"tolower_l", (uintptr_t)&stub_tolower_l},  // <<< IMPLEMENTAR
  {"toupper", (uintptr_t)&toupper},  // pass
  // TODO {"toupper_l", (uintptr_t)&stub_toupper_l},  // <<< IMPLEMENTAR
  // TODO {"towlower_l", (uintptr_t)&stub_towlower_l},  // <<< IMPLEMENTAR
  // TODO {"towupper_l", (uintptr_t)&stub_towupper_l},  // <<< IMPLEMENTAR
  // TODO {"umask", (uintptr_t)&stub_umask},  // <<< IMPLEMENTAR
  // TODO {"ungetc", (uintptr_t)&stub_ungetc},  // <<< IMPLEMENTAR
  // TODO {"uselocale", (uintptr_t)&stub_uselocale},  // <<< IMPLEMENTAR
  {"usleep", (uintptr_t)&usleep},  // pass
  // TODO {"vasprintf", (uintptr_t)&stub_vasprintf},  // <<< IMPLEMENTAR
  // TODO {"vfprintf", (uintptr_t)&stub_vfprintf},  // <<< IMPLEMENTAR
  {"vsnprintf", (uintptr_t)&vsnprintf},  // pass
  // TODO {"vsscanf", (uintptr_t)&stub_vsscanf},  // <<< IMPLEMENTAR
  // TODO {"waitpid", (uintptr_t)&stub_waitpid},  // <<< IMPLEMENTAR
  // TODO {"wcrtomb", (uintptr_t)&stub_wcrtomb},  // <<< IMPLEMENTAR
  // TODO {"wcscoll_l", (uintptr_t)&stub_wcscoll_l},  // <<< IMPLEMENTAR
  // TODO {"wcslen", (uintptr_t)&stub_wcslen},  // <<< IMPLEMENTAR
  // TODO {"wcsnrtombs", (uintptr_t)&stub_wcsnrtombs},  // <<< IMPLEMENTAR
  // TODO {"wcstod", (uintptr_t)&stub_wcstod},  // <<< IMPLEMENTAR
  // TODO {"wcstof", (uintptr_t)&stub_wcstof},  // <<< IMPLEMENTAR
  // TODO {"wcstol", (uintptr_t)&stub_wcstol},  // <<< IMPLEMENTAR
  // TODO {"wcstold", (uintptr_t)&stub_wcstold},  // <<< IMPLEMENTAR
  // TODO {"wcstoll", (uintptr_t)&stub_wcstoll},  // <<< IMPLEMENTAR
  // TODO {"wcstoul", (uintptr_t)&stub_wcstoul},  // <<< IMPLEMENTAR
  // TODO {"wcstoull", (uintptr_t)&stub_wcstoull},  // <<< IMPLEMENTAR
  // TODO {"wcsxfrm_l", (uintptr_t)&stub_wcsxfrm_l},  // <<< IMPLEMENTAR
  // TODO {"wctob", (uintptr_t)&stub_wctob},  // <<< IMPLEMENTAR
  // TODO {"wmemchr", (uintptr_t)&stub_wmemchr},  // <<< IMPLEMENTAR
  // TODO {"wmemcmp", (uintptr_t)&stub_wmemcmp},  // <<< IMPLEMENTAR
  // TODO {"wmemcpy", (uintptr_t)&stub_wmemcpy},  // <<< IMPLEMENTAR
  // TODO {"wmemmove", (uintptr_t)&stub_wmemmove},  // <<< IMPLEMENTAR
  // TODO {"wmemset", (uintptr_t)&stub_wmemset},  // <<< IMPLEMENTAR
  {"write", (uintptr_t)&write},  // pass
  // TODO {"_ZN4FMOD12ChannelGroup10getChannelEiPPNS_7ChannelE", (uintptr_t)&stub__ZN4FMOD12ChannelGroup10getChannelEiPPNS_7ChannelE},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD12ChannelGroup14getNumChannelsEPi", (uintptr_t)&stub__ZN4FMOD12ChannelGroup14getNumChannelsEPi},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD12ChannelGroup7releaseEv", (uintptr_t)&stub__ZN4FMOD12ChannelGroup7releaseEv},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD12ChannelGroup8addGroupEPS0_bPPNS_13DSPConnectionE", (uintptr_t)&stub__ZN4FMOD12ChannelGroup8addGroupEPS0_bPPNS_13DSPConnectionE},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD14ChannelControl10getNumDSPsEPi", (uintptr_t)&stub__ZN4FMOD14ChannelControl10getNumDSPsEPi},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD14ChannelControl11getDSPClockEPyS1_", (uintptr_t)&stub__ZN4FMOD14ChannelControl11getDSPClockEPyS1_},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD14ChannelControl11getUserDataEPPv", (uintptr_t)&stub__ZN4FMOD14ChannelControl11getUserDataEPPv},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD14ChannelControl11setCallbackEPF11FMOD_RESULTP19FMOD_CHANNELCONTROL24FMOD_CHANNELCONTROL_TYPE33FMOD_CHANNELCONTROL_CALLBACK_TYPEPvS6_E", (uintptr_t)&stub__ZN4FMOD14ChannelControl11setCallbackEPF11FMOD_RESULTP19FMOD_CHANNELCONTROL24FMOD_CHANNELCONTROL_TYPE33FMOD_CHANNELCONTROL_CALLBACK_TYPEPvS6_E},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD14ChannelControl11setUserDataEPv", (uintptr_t)&stub__ZN4FMOD14ChannelControl11setUserDataEPv},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD14ChannelControl12addFadePointEyf", (uintptr_t)&stub__ZN4FMOD14ChannelControl12addFadePointEyf},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD14ChannelControl13setVolumeRampEb", (uintptr_t)&stub__ZN4FMOD14ChannelControl13setVolumeRampEb},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD14ChannelControl4stopEv", (uintptr_t)&stub__ZN4FMOD14ChannelControl4stopEv},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD14ChannelControl6addDSPEiPNS_3DSPE", (uintptr_t)&stub__ZN4FMOD14ChannelControl6addDSPEiPNS_3DSPE},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD14ChannelControl6getDSPEiPPNS_3DSPE", (uintptr_t)&stub__ZN4FMOD14ChannelControl6getDSPEiPPNS_3DSPE},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD14ChannelControl8getPitchEPf", (uintptr_t)&stub__ZN4FMOD14ChannelControl8getPitchEPf},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD14ChannelControl8setDelayEyyb", (uintptr_t)&stub__ZN4FMOD14ChannelControl8setDelayEyyb},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD14ChannelControl8setPitchEf", (uintptr_t)&stub__ZN4FMOD14ChannelControl8setPitchEf},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD14ChannelControl9getPausedEPb", (uintptr_t)&stub__ZN4FMOD14ChannelControl9getPausedEPb},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD14ChannelControl9getVolumeEPf", (uintptr_t)&stub__ZN4FMOD14ChannelControl9getVolumeEPf},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD14ChannelControl9removeDSPEPNS_3DSPE", (uintptr_t)&stub__ZN4FMOD14ChannelControl9removeDSPEPNS_3DSPE},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD14ChannelControl9setPausedEb", (uintptr_t)&stub__ZN4FMOD14ChannelControl9setPausedEb},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD14ChannelControl9setVolumeEf", (uintptr_t)&stub__ZN4FMOD14ChannelControl9setVolumeEf},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD3DSP15getMeteringInfoEP22FMOD_DSP_METERING_INFOS2_", (uintptr_t)&stub__ZN4FMOD3DSP15getMeteringInfoEP22FMOD_DSP_METERING_INFOS2_},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD3DSP16setParameterBoolEib", (uintptr_t)&stub__ZN4FMOD3DSP16setParameterBoolEib},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD3DSP17setParameterFloatEif", (uintptr_t)&stub__ZN4FMOD3DSP17setParameterFloatEif},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD3DSP18setMeteringEnabledEbb", (uintptr_t)&stub__ZN4FMOD3DSP18setMeteringEnabledEbb},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD3DSP7releaseEv", (uintptr_t)&stub__ZN4FMOD3DSP7releaseEv},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD5Sound12getOpenStateEP14FMOD_OPENSTATEPjPbS4_", (uintptr_t)&stub__ZN4FMOD5Sound12getOpenStateEP14FMOD_OPENSTATEPjPbS4_},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD5Sound12setLoopCountEi", (uintptr_t)&stub__ZN4FMOD5Sound12setLoopCountEi},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD5Sound7releaseEv", (uintptr_t)&stub__ZN4FMOD5Sound7releaseEv},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD5Sound9getLengthEPjj", (uintptr_t)&stub__ZN4FMOD5Sound9getLengthEPjj},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD6System10getVersionEPj", (uintptr_t)&stub__ZN4FMOD6System10getVersionEPj},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD6System11createSoundEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE", (uintptr_t)&stub__ZN4FMOD6System11createSoundEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD6System11getCPUUsageEP14FMOD_CPU_USAGE", (uintptr_t)&stub__ZN4FMOD6System11getCPUUsageEP14FMOD_CPU_USAGE},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD6System11mixerResumeEv", (uintptr_t)&stub__ZN4FMOD6System11mixerResumeEv},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD6System12createStreamEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE", (uintptr_t)&stub__ZN4FMOD6System12createStreamEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD6System12mixerSuspendEv", (uintptr_t)&stub__ZN4FMOD6System12mixerSuspendEv},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD6System15createDSPByTypeE13FMOD_DSP_TYPEPPNS_3DSPE", (uintptr_t)&stub__ZN4FMOD6System15createDSPByTypeE13FMOD_DSP_TYPEPPNS_3DSPE},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD6System16getDSPBufferSizeEPjPi", (uintptr_t)&stub__ZN4FMOD6System16getDSPBufferSizeEPjPi},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD6System16setDSPBufferSizeEji", (uintptr_t)&stub__ZN4FMOD6System16setDSPBufferSizeEji},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD6System17getSoftwareFormatEPiP16FMOD_SPEAKERMODES1_", (uintptr_t)&stub__ZN4FMOD6System17getSoftwareFormatEPiP16FMOD_SPEAKERMODES1_},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD6System17setSoftwareFormatEi16FMOD_SPEAKERMODEi", (uintptr_t)&stub__ZN4FMOD6System17setSoftwareFormatEi16FMOD_SPEAKERMODEi},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD6System18createChannelGroupEPKcPPNS_12ChannelGroupE", (uintptr_t)&stub__ZN4FMOD6System18createChannelGroupEPKcPPNS_12ChannelGroupE},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD6System19getStreamBufferSizeEPjS1_", (uintptr_t)&stub__ZN4FMOD6System19getStreamBufferSizeEPjS1_},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD6System4initEijPv", (uintptr_t)&stub__ZN4FMOD6System4initEijPv},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD6System5closeEv", (uintptr_t)&stub__ZN4FMOD6System5closeEv},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD6System6updateEv", (uintptr_t)&stub__ZN4FMOD6System6updateEv},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD6System7lockDSPEv", (uintptr_t)&stub__ZN4FMOD6System7lockDSPEv},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD6System7releaseEv", (uintptr_t)&stub__ZN4FMOD6System7releaseEv},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD6System9playSoundEPNS_5SoundEPNS_12ChannelGroupEbPPNS_7ChannelE", (uintptr_t)&stub__ZN4FMOD6System9playSoundEPNS_5SoundEPNS_12ChannelGroupEbPPNS_7ChannelE},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD6System9unlockDSPEv", (uintptr_t)&stub__ZN4FMOD6System9unlockDSPEv},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD7Channel11getPositionEPjj", (uintptr_t)&stub__ZN4FMOD7Channel11getPositionEPjj},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD7Channel11setPositionEjj", (uintptr_t)&stub__ZN4FMOD7Channel11setPositionEjj},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD7Channel12setLoopCountEi", (uintptr_t)&stub__ZN4FMOD7Channel12setLoopCountEi},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD7Channel13getLoopPointsEPjjS1_j", (uintptr_t)&stub__ZN4FMOD7Channel13getLoopPointsEPjjS1_j},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD7Channel13setLoopPointsEjjjj", (uintptr_t)&stub__ZN4FMOD7Channel13setLoopPointsEjjjj},  // <<< IMPLEMENTAR
  // TODO {"_ZN4FMOD7Channel15getCurrentSoundEPPNS_5SoundE", (uintptr_t)&stub__ZN4FMOD7Channel15getCurrentSoundEPPNS_5SoundE},  // <<< IMPLEMENTAR
};
const int dynlib_functions_count = sizeof(dynlib_functions)/sizeof(dynlib_functions[0]);

// ===================== SIMBOLOS A IMPLEMENTAR =====================
//   accept
//   acosf
//   alarm
//   android_set_abort_message
//   asinf
//   asinhf
//   atanf
//   basename
//   bind
//   btowc
//   chmod
//   clock
//   closelog
//   connect
//   __ctype_get_mb_cur_max
//   difftime
//   dlclose
//   dlerror
//   dl_iterate_phdr
//   dlopen
//   dlsym
//   execl
//   feof
//   ferror
//   FMOD_Channel_GetDelay
//   FMOD_Channel_GetDSPClock
//   FMOD_Channel_GetFadePoints
//   FMOD_Channel_RemoveFadePoints
//   FMOD_Debug_Initialize
//   FMOD_Memory_GetStats
//   FMOD_System_Create
//   FMOD_System_LockDSP
//   FMOD_System_UnlockDSP
//   fork
//   freeaddrinfo
//   freelocale
//   fseeko
//   ftello
//   gai_strerror
//   getaddrinfo
//   getauxval
//   getegid
//   geteuid
//   getgid
//   gethostbyname
//   gethostname
//   getnameinfo
//   getpeername
//   getpwuid
//   getpwuid_r
//   getsockname
//   getsockopt
//   getuid
//   gmtime_r
//   if_nametoindex
//   inet_ntop
//   inet_pton
//   initgroups
//   isascii
//   isdigit_l
//   isgraph
//   islower_l
//   __isnanf
//   isprint
//   isupper_l
//   iswalpha_l
//   iswblank_l
//   iswcntrl_l
//   iswdigit_l
//   iswlower_l
//   iswprint_l
//   iswpunct_l
//   iswspace_l
//   iswupper_l
//   iswxdigit_l
//   isxdigit
//   isxdigit_l
//   kill
//   listen
//   longjmp
//   lround
//   mbrlen
//   mbrtowc
//   mbsnrtowcs
//   mbsrtowcs
//   mbtowc
//   memrchr
//   mlock
//   newlocale
//   openlog
//   poll
//   recv
//   recvfrom
//   remove
//   send
//   sendto
//   setbuf
//   setgid
//   setjmp
//   setsockopt
//   setuid
//   __sF
//   shutdown
//   sigaction
//   sigaddset
//   sigdelset
//   sigemptyset
//   sigfillset
//   siglongjmp
//   signal
//   __signbit
//   sigprocmask
//   sigsetjmp
//   socket
//   socketpair
//   strcoll_l
//   strcspn
//   strerror_r
//   strftime_l
//   strpbrk
//   strspn
//   strtok_r
//   strtold
//   strtold_l
//   strtoll
//   strtoll_l
//   strtoull
//   strtoull_l
//   strxfrm_l
//   swprintf
//   syslog
//   tanhf
//   tcgetattr
//   tcsetattr
//   tolower_l
//   toupper_l
//   towlower_l
//   towupper_l
//   umask
//   ungetc
//   uselocale
//   vasprintf
//   vfprintf
//   vsscanf
//   waitpid
//   wcrtomb
//   wcscoll_l
//   wcslen
//   wcsnrtombs
//   wcstod
//   wcstof
//   wcstol
//   wcstold
//   wcstoll
//   wcstoul
//   wcstoull
//   wcsxfrm_l
//   wctob
//   wmemchr
//   wmemcmp
//   wmemcpy
//   wmemmove
//   wmemset
//   _ZN4FMOD12ChannelGroup10getChannelEiPPNS_7ChannelE
//   _ZN4FMOD12ChannelGroup14getNumChannelsEPi
//   _ZN4FMOD12ChannelGroup7releaseEv
//   _ZN4FMOD12ChannelGroup8addGroupEPS0_bPPNS_13DSPConnectionE
//   _ZN4FMOD14ChannelControl10getNumDSPsEPi
//   _ZN4FMOD14ChannelControl11getDSPClockEPyS1_
//   _ZN4FMOD14ChannelControl11getUserDataEPPv
//   _ZN4FMOD14ChannelControl11setCallbackEPF11FMOD_RESULTP19FMOD_CHANNELCONTROL24FMOD_CHANNELCONTROL_TYPE33FMOD_CHANNELCONTROL_CALLBACK_TYPEPvS6_E
//   _ZN4FMOD14ChannelControl11setUserDataEPv
//   _ZN4FMOD14ChannelControl12addFadePointEyf
//   _ZN4FMOD14ChannelControl13setVolumeRampEb
//   _ZN4FMOD14ChannelControl4stopEv
//   _ZN4FMOD14ChannelControl6addDSPEiPNS_3DSPE
//   _ZN4FMOD14ChannelControl6getDSPEiPPNS_3DSPE
//   _ZN4FMOD14ChannelControl8getPitchEPf
//   _ZN4FMOD14ChannelControl8setDelayEyyb
//   _ZN4FMOD14ChannelControl8setPitchEf
//   _ZN4FMOD14ChannelControl9getPausedEPb
//   _ZN4FMOD14ChannelControl9getVolumeEPf
//   _ZN4FMOD14ChannelControl9removeDSPEPNS_3DSPE
//   _ZN4FMOD14ChannelControl9setPausedEb
//   _ZN4FMOD14ChannelControl9setVolumeEf
//   _ZN4FMOD3DSP15getMeteringInfoEP22FMOD_DSP_METERING_INFOS2_
//   _ZN4FMOD3DSP16setParameterBoolEib
//   _ZN4FMOD3DSP17setParameterFloatEif
//   _ZN4FMOD3DSP18setMeteringEnabledEbb
//   _ZN4FMOD3DSP7releaseEv
//   _ZN4FMOD5Sound12getOpenStateEP14FMOD_OPENSTATEPjPbS4_
//   _ZN4FMOD5Sound12setLoopCountEi
//   _ZN4FMOD5Sound7releaseEv
//   _ZN4FMOD5Sound9getLengthEPjj
//   _ZN4FMOD6System10getVersionEPj
//   _ZN4FMOD6System11createSoundEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE
//   _ZN4FMOD6System11getCPUUsageEP14FMOD_CPU_USAGE
//   _ZN4FMOD6System11mixerResumeEv
//   _ZN4FMOD6System12createStreamEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE
//   _ZN4FMOD6System12mixerSuspendEv
//   _ZN4FMOD6System15createDSPByTypeE13FMOD_DSP_TYPEPPNS_3DSPE
//   _ZN4FMOD6System16getDSPBufferSizeEPjPi
//   _ZN4FMOD6System16setDSPBufferSizeEji
//   _ZN4FMOD6System17getSoftwareFormatEPiP16FMOD_SPEAKERMODES1_
//   _ZN4FMOD6System17setSoftwareFormatEi16FMOD_SPEAKERMODEi
//   _ZN4FMOD6System18createChannelGroupEPKcPPNS_12ChannelGroupE
//   _ZN4FMOD6System19getStreamBufferSizeEPjS1_
//   _ZN4FMOD6System4initEijPv
//   _ZN4FMOD6System5closeEv
//   _ZN4FMOD6System6updateEv
//   _ZN4FMOD6System7lockDSPEv
//   _ZN4FMOD6System7releaseEv
//   _ZN4FMOD6System9playSoundEPNS_5SoundEPNS_12ChannelGroupEbPPNS_7ChannelE
//   _ZN4FMOD6System9unlockDSPEv
//   _ZN4FMOD7Channel11getPositionEPjj
//   _ZN4FMOD7Channel11setPositionEjj
//   _ZN4FMOD7Channel12setLoopCountEi
//   _ZN4FMOD7Channel13getLoopPointsEPjjS1_j
//   _ZN4FMOD7Channel13setLoopPointsEjjjj
//   _ZN4FMOD7Channel15getCurrentSoundEPPNS_5SoundE
