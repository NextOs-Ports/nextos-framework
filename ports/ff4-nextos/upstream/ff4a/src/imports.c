/*
 * imports.c -- shims p/ os imports do libff4a que o glibc não resolve sozinho.
 *
 * Escrito do zero p/ o FF4 (só o que o readelf provou que ele importa):
 *  - ponte stdio bionic<->glibc: __sF (stdin/out/err) + fwrite/fprintf/... que
 *    detectam o FILE* de stream padrão e redirecionam pro glibc.
 *  - __errno (bionic) -> __errno_location (glibc).
 *  - __android_log_print / __system_property_get / android_set_abort_message.
 *  - AAsset* e OpenSL: stubs fracos aqui; obb_data.c (U4) e opensles_shim.c (U5)
 *    fornecem os reais via weak override.
 */
#define _GNU_SOURCE
#include "imports.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ---- ponte stdio: __sF é um buffer; os 3 primeiros "FILE" (stdin/out/err) do
 * bionic caem aqui. Nossos shims de saída detectam o range e usam o glibc. ---- */
char ff4a_sF[0x600]; /* generoso: 3 * sizeof(bionic FILE) c/ folga */

static inline int is_std_stream(void *fp) {
  return (char *)fp >= ff4a_sF && (char *)fp < ff4a_sF + sizeof(ff4a_sF);
}
/* mapeia o stream bionic -> glibc: [0]=stdin [1]=stdout resto=stderr. */
static FILE *map_std(void *fp) {
  size_t off = (char *)fp - ff4a_sF;
  size_t stride = sizeof(ff4a_sF) / 3;
  size_t idx = off / stride;
  if (idx == 0)
    return stdin;
  if (idx == 1)
    return stdout;
  return stderr;
}

static size_t sh_fwrite(const void *p, size_t sz, size_t n, void *fp) {
  if (is_std_stream(fp))
    return fwrite(p, sz, n, map_std(fp));
  return fwrite(p, sz, n, (FILE *)fp);
}
static int sh_fputc(int c, void *fp) {
  if (is_std_stream(fp))
    return fputc(c, map_std(fp));
  return fputc(c, (FILE *)fp);
}
static int sh_fputs(const char *s, void *fp) {
  if (is_std_stream(fp))
    return fputs(s, map_std(fp));
  return fputs(s, (FILE *)fp);
}
static int sh_fflush(void *fp) {
  if (!fp || is_std_stream(fp))
    return fflush(fp ? map_std(fp) : NULL);
  return fflush((FILE *)fp);
}
static int sh_vfprintf(void *fp, const char *fmt, va_list ap) {
  FILE *out = is_std_stream(fp) ? map_std(fp) : (FILE *)fp;
  return vfprintf(out, fmt, ap);
}
static int sh_fprintf(void *fp, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = sh_vfprintf(fp, fmt, ap);
  va_end(ap);
  return r;
}
static int sh_fputc_alias(int c, void *fp) { return sh_fputc(c, fp); }

/* ---- fortify _chk bionic-only (glibc não tem estes) ---- */
static char *sh_strncpy_chk2(char *d, const char *s, size_t n, size_t dl, size_t sl) {
  (void)dl; (void)sl;
  return strncpy(d, s, n);
}
static size_t sh_strlen_chk(const char *s, size_t sl) { (void)sl; return strlen(s); }
static char *sh_strchr_chk(const char *s, int c, size_t sl) { (void)sl; return strchr(s, c); }
static char *sh_strrchr_chk(const char *s, int c, size_t sl) { (void)sl; return strrchr(s, c); }

/* ---- trace de fopen (saves via libc): loga path+modo ---- */
static FILE *sh_fopen(const char *path, const char *mode) {
  FILE *f = fopen(path, mode);
  if (getenv("FF4A_IO_DEBUG"))
    debugPrintf("[IO] fopen(\"%s\", \"%s\") -> %s\n", path ? path : "(null)",
                mode ? mode : "?", f ? "OK" : "FALHOU");
  return f;
}

/* ---- bionicismos ---- */
static volatile int *sh_errno(void) { return (volatile int *)__errno_location(); }

static int sh_android_log_print(int prio, const char *tag, const char *fmt, ...) {
  (void)prio;
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  debugPrintf("[AND:%s] %s\n", tag ? tag : "?", buf);
  return 0;
}
static int sh_system_property_get(const char *name, char *value) {
  (void)name;
  if (value)
    value[0] = 0;
  return 0;
}
static void sh_set_abort_message(const char *msg) {
  debugPrintf("[abort_message] %s\n", msg ? msg : "(null)");
}

/* ---- AAsset: stubs fracos (obb_data.c substitui via weak) ---- */
__attribute__((weak)) void *AAssetManager_fromJava(void *env, void *obj) {
  (void)env; (void)obj;
  return (void *)0x1; /* handle fake não-nulo */
}
__attribute__((weak)) void *AAssetManager_open(void *mgr, const char *fn, int mode) {
  (void)mgr; (void)mode;
  debugPrintf("[AAsset] open(%s) -> stub NULL\n", fn ? fn : "?");
  return NULL;
}
__attribute__((weak)) int AAsset_read(void *a, void *buf, size_t n) {
  (void)a; (void)buf; (void)n; return 0;
}
__attribute__((weak)) long AAsset_seek(void *a, long off, int whence) {
  (void)a; (void)off; (void)whence; return -1;
}
__attribute__((weak)) long AAsset_getLength(void *a) { (void)a; return 0; }
__attribute__((weak)) void AAsset_close(void *a) { (void)a; }

/* ---- OpenSL: stubs fracos (opensles_shim.c substitui via weak) ---- */
__attribute__((weak)) int slCreateEngine(void *a, unsigned b, const void *c,
                                         unsigned d, const void *e, const void *f) {
  (void)b; (void)c; (void)d; (void)e; (void)f;
  if (a) *(void **)a = NULL; /* não deixa o SLObjectItf indefinido */
  debugPrintf("[SL] slCreateEngine stub -> FEATURE_UNSUPPORTED\n");
  return 12; /* SL_RESULT_FEATURE_UNSUPPORTED: engine deve pular áudio */
}
/* SL_IID_*: GUIDs de interface (dados). Todos = dummy, EXCETO SL_IID_ENGINE que
 * o opensles_shim.c define distinto (GetInterface diferencia engine). */
unsigned char ff4a_dummy_iid[16];
/* variáveis-ponteiro distintas do opensles_shim.c (GetInterface diferencia) */
extern void *ff4a_iid_engine, *ff4a_iid_play, *ff4a_iid_bq;

#define IMP(n, f) { n, (uintptr_t)(f) }

DynLibFunction ff4a_imports[] = {
    /* stdio bridge */
    IMP("__sF", ff4a_sF),
    IMP("fopen", sh_fopen),
    IMP("fwrite", sh_fwrite),
    IMP("fputc", sh_fputc_alias),
    IMP("fputs", sh_fputs),
    IMP("fflush", sh_fflush),
    IMP("fprintf", sh_fprintf),
    IMP("vfprintf", sh_vfprintf),
    /* fortify _chk bionic-only */
    IMP("__strncpy_chk2", sh_strncpy_chk2),
    IMP("__strlen_chk", sh_strlen_chk),
    IMP("__strchr_chk", sh_strchr_chk),
    IMP("__strrchr_chk", sh_strrchr_chk),
    /* bionicismos */
    IMP("__errno", sh_errno),
    IMP("__android_log_print", sh_android_log_print),
    IMP("__system_property_get", sh_system_property_get),
    IMP("android_set_abort_message", sh_set_abort_message),
    /* SL_IID_* (dummy até U5) */
    IMP("SL_IID_3DCOMMIT", &ff4a_dummy_iid),
    IMP("SL_IID_3DDOPPLER", &ff4a_dummy_iid),
    IMP("SL_IID_3DGROUPING", &ff4a_dummy_iid),
    IMP("SL_IID_3DLOCATION", &ff4a_dummy_iid),
    IMP("SL_IID_3DMACROSCOPIC", &ff4a_dummy_iid),
    IMP("SL_IID_3DSOURCE", &ff4a_dummy_iid),
    IMP("SL_IID_ANDROIDCONFIGURATION", &ff4a_dummy_iid),
    IMP("SL_IID_ANDROIDEFFECT", &ff4a_dummy_iid),
    IMP("SL_IID_ANDROIDEFFECTCAPABILITIES", &ff4a_dummy_iid),
    IMP("SL_IID_ANDROIDEFFECTSEND", &ff4a_dummy_iid),
    IMP("SL_IID_ANDROIDSIMPLEBUFFERQUEUE", &ff4a_iid_bq),
    IMP("SL_IID_AUDIODECODERCAPABILITIES", &ff4a_dummy_iid),
    IMP("SL_IID_AUDIOENCODER", &ff4a_dummy_iid),
    IMP("SL_IID_AUDIOENCODERCAPABILITIES", &ff4a_dummy_iid),
    IMP("SL_IID_AUDIOIODEVICECAPABILITIES", &ff4a_dummy_iid),
    IMP("SL_IID_BASSBOOST", &ff4a_dummy_iid),
    IMP("SL_IID_BUFFERQUEUE", &ff4a_iid_bq),
    IMP("SL_IID_DEVICEVOLUME", &ff4a_dummy_iid),
    IMP("SL_IID_DYNAMICINTERFACEMANAGEMENT", &ff4a_dummy_iid),
    IMP("SL_IID_DYNAMICSOURCE", &ff4a_dummy_iid),
    IMP("SL_IID_EFFECTSEND", &ff4a_dummy_iid),
    IMP("SL_IID_ENGINE", &ff4a_iid_engine),
    IMP("SL_IID_ENGINECAPABILITIES", &ff4a_dummy_iid),
    IMP("SL_IID_ENVIRONMENTALREVERB", &ff4a_dummy_iid),
    IMP("SL_IID_EQUALIZER", &ff4a_dummy_iid),
    IMP("SL_IID_LED", &ff4a_dummy_iid),
    IMP("SL_IID_METADATAEXTRACTION", &ff4a_dummy_iid),
    IMP("SL_IID_METADATATRAVERSAL", &ff4a_dummy_iid),
    IMP("SL_IID_MIDIMESSAGE", &ff4a_dummy_iid),
    IMP("SL_IID_MIDIMUTESOLO", &ff4a_dummy_iid),
    IMP("SL_IID_MIDITEMPO", &ff4a_dummy_iid),
    IMP("SL_IID_MIDITIME", &ff4a_dummy_iid),
    IMP("SL_IID_MUTESOLO", &ff4a_dummy_iid),
    IMP("SL_IID_NULL", &ff4a_dummy_iid),
    IMP("SL_IID_OBJECT", &ff4a_dummy_iid),
    IMP("SL_IID_OUTPUTMIX", &ff4a_dummy_iid),
    IMP("SL_IID_PITCH", &ff4a_dummy_iid),
    IMP("SL_IID_PLAY", &ff4a_iid_play),
    IMP("SL_IID_PLAYBACKRATE", &ff4a_dummy_iid),
    IMP("SL_IID_PREFETCHSTATUS", &ff4a_dummy_iid),
    IMP("SL_IID_PRESETREVERB", &ff4a_dummy_iid),
    IMP("SL_IID_RATEPITCH", &ff4a_dummy_iid),
    IMP("SL_IID_RECORD", &ff4a_dummy_iid),
    IMP("SL_IID_SEEK", &ff4a_dummy_iid),
    IMP("SL_IID_THREADSYNC", &ff4a_dummy_iid),
    IMP("SL_IID_VIBRA", &ff4a_dummy_iid),
    IMP("SL_IID_VIRTUALIZER", &ff4a_dummy_iid),
    IMP("SL_IID_VISUALIZATION", &ff4a_dummy_iid),
    IMP("SL_IID_VOLUME", &ff4a_dummy_iid),
    /* AAsset (weak; U4) */
    IMP("AAssetManager_fromJava", AAssetManager_fromJava),
    IMP("AAssetManager_open", AAssetManager_open),
    IMP("AAsset_read", AAsset_read),
    IMP("AAsset_seek", AAsset_seek),
    IMP("AAsset_getLength", AAsset_getLength),
    IMP("AAsset_close", AAsset_close),
    /* OpenSL (weak; U5) */
    IMP("slCreateEngine", slCreateEngine),
};
int ff4a_imports_count = (int)(sizeof(ff4a_imports) / sizeof(ff4a_imports[0]));
