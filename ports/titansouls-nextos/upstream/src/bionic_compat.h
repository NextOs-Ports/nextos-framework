#ifndef ASM2_BIONIC_COMPAT_H
#define ASM2_BIONIC_COMPAT_H

#include <dirent.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <wchar.h>

#ifndef ASM2_GUEST_PCS
#if defined(__arm__)
#define ASM2_GUEST_PCS __attribute__((pcs("aapcs")))
#else
/*
 * Android x86 and Linux i386 both use SysV cdecl.  In particular, x87/SSE
 * floating-point arguments do not need the ARM softfp bridge.
 */
#define ASM2_GUEST_PCS
#endif
#endif

/* Imported Android/Bionic data objects.  The ctype exports are pointer
 * variables, not direct aliases to the backing arrays. */
extern const char *asm2_ctype_ptr;
extern const short *asm2_tolower_ptr;
extern const short *asm2_toupper_ptr;
extern int asm2_page_size;
extern uintptr_t asm2_stack_chk_guard;
extern unsigned char asm2_bionic_sF[3][84];

struct asm2_bionic_file_stats {
  uint64_t created;
  uint64_t open;
  uint64_t closed;
  uint32_t longest_active_bucket;
};

void asm2_bionic_init(const char *storage_root);
void asm2_bionic_get_file_stats(struct asm2_bionic_file_stats *stats);
const char *asm2_translate_path(const char *path, char *buffer,
                                size_t buffer_size);

int *ASM2_GUEST_PCS asm2_errno(void);
int ASM2_GUEST_PCS asm2_android_log_print(int priority, const char *tag,
                                           const char *format, ...);
int ASM2_GUEST_PCS asm2_android_log_vprint(int priority, const char *tag,
                                            const char *format, va_list args);
void ASM2_GUEST_PCS asm2_assert2(const char *file, int line,
                                  const char *function,
                                  const char *expression);

void ASM2_GUEST_PCS asm2_clearerr(void *stream);
int ASM2_GUEST_PCS asm2_fclose(void *stream);
void *ASM2_GUEST_PCS asm2_fdopen(int fd, const char *mode);
int ASM2_GUEST_PCS asm2_fflush(void *stream);
char *ASM2_GUEST_PCS asm2_fgets(char *text, int size, void *stream);
void *ASM2_GUEST_PCS asm2_fopen(const char *path, const char *mode);
int ASM2_GUEST_PCS asm2_fprintf(void *stream, const char *format, ...);
int ASM2_GUEST_PCS asm2_fputc(int character, void *stream);
int ASM2_GUEST_PCS asm2_fputs(const char *text, void *stream);
size_t ASM2_GUEST_PCS asm2_fread(void *data, size_t size, size_t count,
                                  void *stream);
int ASM2_GUEST_PCS asm2_fseek(void *stream, long offset, int origin);
int ASM2_GUEST_PCS asm2_fseeko(void *stream, off_t offset, int origin);
long ASM2_GUEST_PCS asm2_ftell(void *stream);
off_t ASM2_GUEST_PCS asm2_ftello(void *stream);
size_t ASM2_GUEST_PCS asm2_fwrite(const void *data, size_t size, size_t count,
                                   void *stream);
int ASM2_GUEST_PCS asm2_getc(void *stream);
wint_t ASM2_GUEST_PCS asm2_getwc(void *stream);
int ASM2_GUEST_PCS asm2_putc(int character, void *stream);
wint_t ASM2_GUEST_PCS asm2_putwc(wchar_t character, void *stream);
int ASM2_GUEST_PCS asm2_setvbuf(void *stream, char *buffer, int mode,
                                 size_t size);
int ASM2_GUEST_PCS asm2_ungetc(int character, void *stream);
wint_t ASM2_GUEST_PCS asm2_ungetwc(wint_t character, void *stream);
int ASM2_GUEST_PCS asm2_vfprintf(void *stream, const char *format,
                                  va_list args);

int ASM2_GUEST_PCS asm2_access(const char *path, int mode);
int ASM2_GUEST_PCS asm2_chdir(const char *path);
int ASM2_GUEST_PCS asm2_chmod(const char *path, mode_t mode);
char *ASM2_GUEST_PCS asm2_getcwd(char *buffer, size_t size);
int ASM2_GUEST_PCS asm2_mkdir(const char *path, mode_t mode);
int ASM2_GUEST_PCS asm2_mkstemp(char *path_template);
int ASM2_GUEST_PCS asm2_open(const char *path, int flags, ...);
void *ASM2_GUEST_PCS asm2_opendir(const char *path);
int ASM2_GUEST_PCS asm2_remove(const char *path);
int ASM2_GUEST_PCS asm2_rename(const char *old_path, const char *new_path);
int ASM2_GUEST_PCS asm2_rmdir(const char *path);
int ASM2_GUEST_PCS asm2_stat(const char *path, void *guest_stat);
int ASM2_GUEST_PCS asm2_fstat(int fd, void *guest_stat);
int ASM2_GUEST_PCS asm2_statfs(const char *path, void *guest_statfs);
int ASM2_GUEST_PCS asm2_unlink(const char *path);
void *ASM2_GUEST_PCS asm2_readdir(void *directory);
int ASM2_GUEST_PCS asm2_sigaction(int signal_number, const void *guest_action,
                                   void *guest_old_action);
int ASM2_GUEST_PCS asm2_strerror_r(int error_number, char *buffer,
                                    size_t buffer_size);
long ASM2_GUEST_PCS asm2_sysconf(int android_name);

#endif
