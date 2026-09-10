#ifndef __BIONIC_SHIMS_H__
#define __BIONIC_SHIMS_H__

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <sys/types.h>

extern unsigned char __sF[];
extern const unsigned char *_ctype_;

int __android_log_print(int prio, const char *tag, const char *fmt, ...);
int __android_log_write(int prio, const char *tag, const char *text);
int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap);

int *bionic_errno(void);
int bionic_system_property_get(const char *name, char *value);

int bionic_sigaction(int sig, const void *act, void *oldact);

int bionic_sem_init(void *s, int pshared, unsigned value);
int bionic_sem_destroy(void *s);
int bionic_sem_trywait(void *s);
int bionic_sem_wait(void *s);
int bionic_sem_post(void *s);
int bionic_sem_getvalue(void *s, int *out);

void *bionic_dlopen(const char *name, int flags);
void *bionic_dlsym(void *handle, const char *name);
int bionic_dlclose(void *handle);
char *bionic_dlerror(void);

#endif
char *__strncpy_chk2(char *dst, const char *src, size_t n, size_t dst_len, size_t src_len);
void __FD_SET_chk(int fd, void *set, size_t setsize);
void __assert2(const char *file, int line, const char *func, const char *expr);
void android_set_abort_message(const char *msg);
size_t __strlen_chk(const char *s, size_t maxlen);

// Stdio shims for __sF mapping
int bb_fflush(void *f);
int bb_fprintf(void *f, const char *fmt, ...);
int bb_vfprintf(void *f, const char *fmt, va_list ap);
size_t bb_fwrite(const void *p, size_t sz, size_t n, void *f);
size_t bb_fread(void *p, size_t sz, size_t n, void *f);
int bb_fputs(const char *s, void *f);
int bb_fputc(int c, void *f);
int bb_ungetc(int c, void *f);
int bb_feof(void *f);
int bb_ferror(void *f);
int bb_fileno(void *f);
int bb_fseek(void *f, long off, int wh);
long bb_ftell(void *f);
char *bb_fgets(char *s, int n, void *f);
int bb_fclose(void *f);
int bb_getc(void *f);
int bb_putc(int c, void *f);
void bb_setbuf(void *f, char *buf);
int bb_setvbuf(void *f, char *buf, int mode, size_t sz);
