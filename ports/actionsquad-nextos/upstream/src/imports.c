/* imports.c (DEVICE) -- Android so-loader import surface (donor-scaffold lineage).
 *
 * Tabela de OVERRIDES (resolvida ANTES do fallback dlsym do so_resolve do
 * so_util AArch64). Tudo que NÃO está aqui cai no dlsym(RTLD_DEFAULT) ->
 * libc/libm/libGLESv2/libEGL/SDL2 pré-carregadas RTLD_GLOBAL.
 *
 *   EGL          -> egl_shim_*   (contexto GLES2 via SDL2, Mali fbdev)
 *   ANativeWindow/AAsset/ALooper extras -> impls locais (faltam no android_shim)
 *   OpenSL ES    -> opensles_shim
 *   bionic _chk/__assert2/property/log -> wrappers
 *   pthread      -> revc_pthread_table (pthread_bridge.c)
 *   libc++       -> snapshot (mesclado no main.c, módulo A)
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>

#include "so_util.h"
#include "egl_shim.h"
#include "android_shim.h"

/* ---------------- liblog ---------------- */
/* O Oboe (OpenSLES) spamma "fireDataCallback() called with data callback
 * disabled!" milhares de vezes (5 streams ativos); cada linha vira fwrite no
 * stderr->tee->log.txt = storm de I/O no SD -> choppy/slideshow. Suprime essa
 * mensagem repetida (loga as 5 primeiras, depois corta). O áudio em si funciona
 * (ring alimentado, underruns=0) — isto só tira o ruído de log. */
static int log_is_spam(const char *s) {
  return s && (strstr(s, "data callback disabled") || strstr(s, "fireDataCallback"));
}
static int b_log_print(int prio, const char *tag, const char *fmt, ...) {
  if (log_is_spam(fmt)) { static int n = 0; if (++n > 5) return 0;
    fprintf(stderr, "[ALOG:%d %s] (Oboe spam suppressed after 5x) ", prio, tag?tag:"?"); }
  else fprintf(stderr, "[ALOG:%d %s] ", prio, tag ? tag : "?");
  va_list ap; va_start(ap, fmt);
  vfprintf(stderr, fmt, ap); fprintf(stderr, "\n"); va_end(ap);
  return 0;
}
static int b_log_write(int prio, const char *tag, const char *text) {
  if (log_is_spam(text)) { static int n = 0; if (++n > 5) return 0; }
  fprintf(stderr, "[ALOG:%d %s] %s\n", prio, tag ? tag : "?", text ? text : "");
  return 0;
}
/* NÃO faz vfprintf dos varargs (o jogo passa %s com ponteiros que podem estar
 * ruins na falha de textura -> memcpy crash) e NÃO aborta (a engine usa
 * __android_log_assert como log não-fatal aqui). */
static void b_log_assert(const char *cond, const char *tag, const char *fmt, ...) {
  (void)fmt;
  fprintf(stderr, "[ALOG-ASSERT %s] %s\n", tag ? tag : "?", cond ? cond : "");
}

/* ---------------- bionic libc ---------------- */
static int *b_errno(void) { extern int *__errno_location(void); return __errno_location(); }
static void b_assert2(const char *f, int l, const char *fn, const char *e) {
  fprintf(stderr, "assert2: %s:%d %s: %s\n", f ? f : "?", l, fn ? fn : "?", e ? e : "?");
  abort();
}
static size_t b_strlen_chk(const char *s, size_t n) { (void)n; return strlen(s); }
static char  *b_strchr_chk(const char *s, int c, size_t n) { (void)n; return strchr(s, c); }
static mode_t b_umask_chk(mode_t m) { return umask(m); }
static int    b_sys_prop_get(const char *name, char *value) {
  /* SDK 25 (N-MR1): Oboe/OpenSLES aceita Float (>=21) e NÃO tenta AAudio
   * (>=27). Sem isso: "ErrorInvalidFormat" no open do stream de som. */
  if (name && value && strcmp(name, "ro.build.version.sdk") == 0) {
    strcpy(value, "25");
    return 2;
  }
  if (value) value[0] = '\0';
  return 0;
}
/* __emutls_get_address vem do libgcc (linkado estático no loader). */
extern void *__emutls_get_address(void *);

/* bionic __sF[3] = stdin/out/err (libc++ usa p/ std::cerr/cout).
 *
 * ABI Android LP64 pré-M: sizeof(FILE) == 152, conforme o struct __sFILE do
 * Bionic. Não é uma linha de 512 bytes por stream: o código convidado calcula
 * &__sF[1]/[2] com passos de 152. Usar 512 fazia map_sF reconhecer só stdin e
 * entregar stdout/stderr Bionic diretamente ao fwrite da glibc; ela lia o
 * campo _lock inexistente e caía em NULL+8 ao carregar publisher.tga. */
#define BIONIC_FILE_SIZE 152u
static _Alignas(8) unsigned char bionic_sF[3 * BIONIC_FILE_SIZE];
static FILE *map_sF(void *fp) {
  uintptr_t p = (uintptr_t)fp;
  uintptr_t base = (uintptr_t)bionic_sF;
  if (p == base) return stdin;
  if (p == base + BIONIC_FILE_SIZE) return stdout;
  if (p == base + 2u * BIONIC_FILE_SIZE) return stderr;
  return (FILE *)fp;
}
static int w_fprintf(void *fp, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt); int r = vfprintf(map_sF(fp), fmt, ap); va_end(ap); return r;
}
static int w_vfprintf(void *fp, const char *fmt, va_list ap) { return vfprintf(map_sF(fp), fmt, ap); }
static size_t w_fwrite(const void *p, size_t s, size_t n, void *fp) { return fwrite(p, s, n, map_sF(fp)); }
static int w_fputs(const char *str, void *fp) { return fputs(str, map_sF(fp)); }
static int w_fputc(int c, void *fp) { return fputc(c, map_sF(fp)); }
static int w_fflush(void *fp) { return fflush(fp ? map_sF(fp) : NULL); }
static void b_set_abort_message(const char *m) { fprintf(stderr, "[abort_msg] %s\n", m ? m : "?"); }

/* setjmp/longjmp: o bionic NÃO salva a sigmask por padrão; mapeamos pros
 * _setjmp/_longjmp do glibc (também sem sigmask) p/ casar a ABI. O jogo usa
 * isso no error-handling do libjpeg (ImageWriterJPEG) -> sem isso, crash. */
extern int _setjmp(void *);
extern void _longjmp(void *, int) __attribute__((noreturn));

/* ---------------- ANativeWindow (faltam no android_shim) ---------------- */
extern int coi_screen_w, coi_screen_h; /* resolucao real (egl_shim) */
#define COI_W coi_screen_w
#define COI_H coi_screen_h
static ANativeWindow *aw_fromSurface(void *env, void *surface) {
  (void)env; (void)surface; return android_shim_get_window();
}
static void aw_acquire(void *w) { (void)w; }
static void aw_release(void *w) { (void)w; }
static int  aw_getWidth(void *w)  { (void)w; return COI_W; }
static int  aw_getHeight(void *w) { (void)w; return COI_H; }

/* ---------------- AAsset / AAssetManager / AAssetDir ----------------
 * O jogo lê assets/*.pak via AAssetManager. Servimos de um diretório real
 * no device (extraído do APK). Base configurável por env, default "assets". */
typedef struct { FILE *fp; long len; char path[512]; } CoiAsset;
typedef struct { DIR *d; } CoiAssetDir;
#include <dirent.h>
#include <fcntl.h>

static const char *assets_base(void) {
  const char *b = getenv("COI_ASSETS");
  return (b && *b) ? b : "assets";
}
static void *aam_fromJava(void *env, void *obj) { (void)env; (void)obj; return (void *)1; }
static void *aam_open(void *mgr, const char *fn, int mode) {
  (void)mgr; (void)mode;
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", assets_base(), fn);
  FILE *fp = fopen(path, "rb");
  if (!fp) { fprintf(stderr, "[AAsset] MISS %s\n", path); return NULL; }
  CoiAsset *a = calloc(1, sizeof(CoiAsset));
  a->fp = fp; fseek(fp, 0, SEEK_END); a->len = ftell(fp); fseek(fp, 0, SEEK_SET);
  snprintf(a->path, sizeof(a->path), "%s", path);
  fprintf(stderr, "[AAsset] open %s len=%ld\n", fn, a->len);
  return a;
}
static int g_ar = 0;
static int    aa_read(void *h, void *buf, size_t n) {
  CoiAsset *a = h; if (!a) return -1;
  long p = ftell(a->fp); int r = (int)fread(buf, 1, n, a->fp);
  if (a->len > 100000000L && g_ar < 30) {
    unsigned char *b = buf;
    fprintf(stderr, "[pak read] pos=%ld n=%zu got=%d b=%02x%02x%02x%02x\n",
            p, n, r, r>0?b[0]:0, r>1?b[1]:0, r>2?b[2]:0, r>3?b[3]:0);
    g_ar++;
  }
  return r;
}
static long   aa_seek(void *h, long off, int wh) {
  CoiAsset *a = h; if (!a) return -1; fseek(a->fp, off, wh);
  if (a->len > 100000000L && g_ar < 30)
    fprintf(stderr, "[pak seek] off=%ld wh=%d -> %ld\n", off, wh, ftell(a->fp));
  return ftell(a->fp);
}
static long   aa_seek64(void *h, long off, int wh)   { return aa_seek(h, off, wh); }
static long   aa_getLength(void *h)   { CoiAsset *a = h; return a ? a->len : 0; }
static long   aa_getRemaining(void *h){ CoiAsset *a = h; return a ? a->len - ftell(a->fp) : 0; }
static void   aa_close(void *h)       { CoiAsset *a = h; if (a) { fclose(a->fp); free(a); } }
static int    aa_openFd(void *h, long *start, long *len) {
  CoiAsset *a = h; if (!a) return -1;
  if (start) *start = 0; if (len) *len = a->len;
  fflush(a->fp);
  int fd = dup(fileno(a->fp));
  fprintf(stderr, "[AAsset] openFd %s len=%ld -> fd=%d\n", a->path, a->len, fd);
  return fd;
}
/* override read() p/ diagnóstico: loga os primeiros reads de fd alto (jogo). */
static long my_read(int fd, void *buf, size_t n) {
  static long (*real)(int, void *, size_t) = NULL;
  if (!real) real = dlsym(RTLD_DEFAULT, "read");
  off_t pos = (fd >= 10) ? lseek(fd, 0, SEEK_CUR) : -1;
  long r = real(fd, buf, n);
  static int c = 0;
  if (fd >= 17 && c < 120) {
    unsigned char *b = buf;
    fprintf(stderr, "[read] fd=%d pos=%ld n=%zu got=%ld b=%02x%02x%02x%02x\n",
            fd, (long)pos, n, r, r>0?b[0]:0, r>1?b[1]:0, r>2?b[2]:0, r>3?b[3]:0);
    c++;
  }
  return r;
}
/* std::ifstream (basic_filebuf::open): o jogo abre o pak de texturas via
 * ifstream com path relativo -> não acha no CWD -> lê lixo (0xffd9). Logamos e
 * redirecionamos pra assets/. g_real_filebuf_open vem do snapshot (main.c). */
void *g_real_filebuf_open = NULL;
static void *my_filebuf_open(void *self, const char *path, unsigned mode) {
  void *(*real)(void *, const char *, unsigned) = g_real_filebuf_open;
  void *r = real ? real(self, path, mode) : NULL;
  static int c = 0;
  if (c < 120) { fprintf(stderr, "[ifstream] '%s' mode=%u -> %s\n", path, mode, r?"OK":"FAIL"); c++; }
  if (!r && path && path[0] != '/') {
    char alt[1024]; snprintf(alt, sizeof(alt), "%s/%s", assets_base(), path);
    r = real ? real(self, alt, mode) : NULL;
    if (r && c < 130) fprintf(stderr, "[ifstream] -> redirect assets/%s OK\n", path);
  }
  return r;
}

/* fread/fseek override p/ diagnóstico: captura leituras de arquivo (texturas). */
static size_t my_fread(void *buf, size_t sz, size_t nm, void *fp) {
  static size_t (*real)(void *, size_t, size_t, void *) = NULL;
  if (!real) real = dlsym(RTLD_DEFAULT, "fread");
  long pos = ftell(fp);
  size_t r = real(buf, sz, nm, fp);
  static int c = 0;
  unsigned char *b = buf;
  /* loga reads GRANDES (JPEG/textura, não entradas de índice) + marcadores JPEG */
  if (c < 60 && (sz*nm >= 1000 || (r>=2 && b[0]==0xff && (b[1]==0xd8||b[1]==0xd9)))) {
    fprintf(stderr, "[fread] fp=%p pos=%ld sz=%zu got=%zu b=%02x%02x%02x%02x\n",
            fp, pos, sz*nm, r, r>0?b[0]:0, b[1], b[2], b[3]);
    c++;
  }
  return r;
}
static int my_fseek(void *fp, long off, int wh) {
  static int (*real)(void *, long, int) = NULL;
  if (!real) real = dlsym(RTLD_DEFAULT, "fseek");
  static int c = 0;
  if (c < 80) { fprintf(stderr, "[fseek] fp=%p off=%ld wh=%d\n", fp, off, wh); c++; }
  return real(fp, off, wh);
}

/* fopen override: loga path; se for arquivo do jogo não-achado no CWD,
 * tenta em assets/ (o jogo pode usar paths relativos/diferentes). */
static void *my_fopen(const char *path, const char *mode) {
  static void *(*real)(const char *, const char *) = NULL;
  if (!real) real = dlsym(RTLD_DEFAULT, "fopen");
  void *fp = real(path, mode);
  static int c = 0;
  /* ESCRITA E FALHA SEMPRE APARECEM. O save do jogo e' um fopen("wb") simples
   * (oz::AndroidStorageService::WriteData -> "%s/%s" de getStorageDir + nome),
   * e o cap de 200 linhas do log de leitura estourava com as texturas ANTES do
   * primeiro save — o relato de campo "nao salva" chegava sem uma unica pista.
   * Escritas sao poucas e valem cada byte de log; falha de leitura idem. */
  int is_write = mode && (mode[0] == 'w' || mode[0] == 'a' || strchr(mode, '+'));
  /* userdata/ = save/config do jogador. Toda abertura ali — leitura E escrita —
   * vai para o log sem cap: e' a evidencia direta de "salvou" e "recarregou"
   * num relato de campo, e sao pouquissimas linhas por sessao. */
  int is_save = path && strstr(path, "userdata/");
  if (is_write || is_save || !fp)
    fprintf(stderr, "[fopen] '%s' %s -> %s%s\n", path, mode, fp ? "OK" : "FAIL",
            is_write ? " (write)" : (is_save ? " (userdata)" : ""));
  else if (c < 200) { fprintf(stderr, "[fopen] '%s' %s -> OK\n", path, mode); c++; }
  if (!fp && path && path[0] != '/') {
    char alt[1024]; snprintf(alt, sizeof(alt), "%s/%s", assets_base(), path);
    fp = real(alt, mode);
    if (fp && c < 70) { fprintf(stderr, "[fopen] -> redirect assets/%s OK\n", path); }
  }
  return fp;
}
/* DIAG dos POOLS da engine: loga alocações GIGANTES (>=16MB) + return-addr p/
 * identificar os pools (StageObjectAllocator/btGenericMemoryPool/General Pool) que
 * dominam a RAM no .127. extern do base da libGame p/ simbolicar o caller. */
/* (DIAG removido 2026-06-16: hooks malloc/calloc/posix_memalign confirmaram que os
 * ~344MB NÃO vêm de alocações grandes únicas — é dado vivo de objetos do mundo via
 * muitas alocações pequenas; nada >=16MB. Não há lever de pool reduzível por shim.) */

/* força stack grande nas threads do jogo: a worker de loading tem cadeia de
 * parsing profunda; a stack pedida (bionic ~1MB) pode estourar sob glibc. */
static int my_attr_setstacksize(void *attr, size_t sz) {
  static int (*real)(void *, size_t) = NULL;
  if (!real) real = dlsym(RTLD_DEFAULT, "pthread_attr_setstacksize");
  /* NOTA (investigado 2026-06-16): os ~344MB de regiões grandes [stack/anon] no .127
   * NÃO vêm daqui (só 3 chamadas de 2MB->8MB); são pools próprios da engine (General
   * Pool/StageObjectAllocatorPage). Capar aqui é no-op. Mantém só o MIN 8MB original. */
  if (sz < 8u * 1024 * 1024) sz = 8u * 1024 * 1024;
  return real(attr, sz);
}
static long my_read_chk(int fd, void *buf, size_t n, size_t buflen) {
  static long (*real)(int, void *, size_t) = NULL;
  if (!real) real = dlsym(RTLD_DEFAULT, "read");
  off_t pos = lseek(fd, 0, SEEK_CUR);
  long r = real(fd, buf, n > buflen ? buflen : n);
  static int c = 0;
  if (c < 50) {
    unsigned char *b = buf;
    fprintf(stderr, "[read_chk] fd=%d pos=%ld n=%zu got=%ld b=%02x%02x%02x%02x\n",
            fd, (long)pos, n, r, r>0?b[0]:0, r>1?b[1]:0, r>2?b[2]:0, r>3?b[3]:0);
    c++;
  }
  return r;
}
static void *aam_openDir(void *mgr, const char *dirn) {
  (void)mgr; char path[1024];
  snprintf(path, sizeof(path), "%s/%s", assets_base(), dirn && *dirn ? dirn : ".");
  DIR *d = opendir(path); if (!d) return NULL;
  CoiAssetDir *ad = calloc(1, sizeof(CoiAssetDir)); ad->d = d; return ad;
}
static const char *aad_getNext(void *h) {
  CoiAssetDir *ad = h; if (!ad) return NULL;
  struct dirent *e;
  while ((e = readdir(ad->d))) { if (e->d_name[0] != '.') return e->d_name; }
  return NULL;
}
static void aad_close(void *h) { CoiAssetDir *ad = h; if (ad) { closedir(ad->d); free(ad); } }

/* ---------------- ALooper extras (faltam no android_shim) ---------------- */
static int  al_pollOnce(int t, int *fd, int *ev, void **data) { return ALooper_pollAll(t, fd, ev, data); }
static void *al_forThread(void) { return ALooper_forThread(); }
static void al_acquire(void *l) { ALooper_acquire((ALooper *)l); }
static void al_release(void *l) { ALooper_release((ALooper *)l); }
static int al_removeFd(void *l, int fd) { return ALooper_removeFd((ALooper *)l, fd); }
static void al_wake(void *l) { ALooper_wake((ALooper *)l); }

/* AInput extras */
static int   aie_getDeviceId(void *e) { (void)e; return 0; }
static int   ame_getButtonState(void *e) { (void)e; return 0; }

/* ---------------- OpenSL ES interface IDs ----------------
 * IDENTIDADES DO SHIM (receita Sonic Mania): o opensles_shim compara iid por
 * ponteiro com os sl_IID_* DELE. Os SL_IID_* expostos ao jogo (tabela+dlsym)
 * têm que conter esses valores; ANDROIDSIMPLEBUFFERQUEUE -> BUFFERQUEUE. */
extern uint32_t slCreateEngine_shim(void **, uint32_t, const void *, uint32_t,
                                    const void *, const void *);
extern const void *sl_IID_ENGINE, *sl_IID_PLAY, *sl_IID_VOLUME,
    *sl_IID_BUFFERQUEUE;
static const void *SL_IID_ENGINE_v, *SL_IID_PLAY_v, *SL_IID_RECORD_v,
    *SL_IID_BUFFERQUEUE_v, *SL_IID_ANDROIDSIMPLEBUFFERQUEUE_v,
    *SL_IID_ANDROIDCONFIGURATION_v = "ACFG";
__attribute__((constructor)) static void sl_iid_init(void) {
  SL_IID_ENGINE_v = sl_IID_ENGINE;
  SL_IID_PLAY_v = sl_IID_PLAY;
  SL_IID_RECORD_v = "RECORD"; /* sem suporte no shim */
  SL_IID_BUFFERQUEUE_v = sl_IID_BUFFERQUEUE;
  SL_IID_ANDROIDSIMPLEBUFFERQUEUE_v = sl_IID_BUFFERQUEUE;
}

/* EGL shim funcs já declaradas em egl_shim.h (assinaturas reais) */
static unsigned egl_releasethread_stub(void) { return 1u; }

/* glGetString: o renderer NX copia GL_EXTENSIONS/GL_VERSION em buffers de pilha
 * fixos (engine feita p/ ES3). A lista REAL do Mali Utgard estoura -> stack smash.
 * Retornamos strings curtas/controladas (ES 2.0 + extensões mínimas). */
static const unsigned char *my_glGetString(unsigned name) {
  /* 🟢 NATIVO ES3 (GLVER=3, ex: R36S/G31): o motor compara o GL_VERSION com o
   * opengl_version do config; se forçarmos "ES 2.0" aqui com config ES3, o renderer
   * NÃO inicializa ("Failed to pre-initialize NEXUS"). Devolvemos strings ES3
   * CURTAS/controladas (sem a lista gigante real que estoura o buffer de pilha do
   * motor). ETC2 é CORE no ES3 (não precisa estar na string). */
  static int es3 = -1;
  if (es3 < 0) { const char *g = getenv("COI_GLVER"); es3 = (g && g[0] == '3') ? 1 : 0; }
  fprintf(stderr, "[my_glGetString] name=0x%x es3=%d\n", name, es3);
  if (es3) {
    switch (name) {
    case 0x1F00: return (const unsigned char *)"ARM";                    /* GL_VENDOR */
    case 0x1F01: return (const unsigned char *)"Mali-G31";               /* GL_RENDERER */
    case 0x1F02: return (const unsigned char *)"OpenGL ES 3.2";          /* GL_VERSION */
    case 0x8B8C: return (const unsigned char *)"OpenGL ES GLSL ES 3.20"; /* GLSL */
    case 0x1F03: return (const unsigned char *)                          /* GL_EXTENSIONS (curado) */
        "GL_OES_texture_npot GL_OES_depth_texture GL_OES_packed_depth_stencil "
        "GL_OES_rgb8_rgba8 GL_OES_element_index_uint GL_OES_vertex_array_object "
        "GL_EXT_texture_format_BGRA8888 GL_OES_compressed_ETC1_RGB8_texture "
        "GL_EXT_color_buffer_half_float GL_OES_texture_half_float";
    default: break;
    }
  }
  switch (name) {
  case 0x1F00: return (const unsigned char *)"NextOS";                 /* GL_VENDOR */
  case 0x1F01: return (const unsigned char *)"Mali-450 (GLES2)";       /* GL_RENDERER */
  case 0x1F02: return (const unsigned char *)"OpenGL ES 2.0";          /* GL_VERSION */
  case 0x8B8C: return (const unsigned char *)"OpenGL ES GLSL ES 1.00"; /* GL_SHADING_LANGUAGE_VERSION */
  case 0x1F03: return (const unsigned char *)                          /* GL_EXTENSIONS */
      "GL_OES_texture_npot GL_OES_depth_texture GL_OES_packed_depth_stencil "
      /* The Mali-450 MP driver on this device does not expose
       * GL_OES_element_index_uint and rejects GL_UNSIGNED_INT indices with
       * GL_INVALID_ENUM.  Advertising it made Action Squad select 32-bit
       * indices for the two level-composition quads, so neither quad wrote a
       * single pixel.  Keep this bounded list truthful: the engine will then
       * follow its native 16-bit-index path. */
      "GL_OES_rgb8_rgba8 GL_OES_vertex_array_object "
      "GL_EXT_texture_format_BGRA8888";
  default: {
    static const unsigned char *(*real)(unsigned) = NULL;
    if (!real) real = dlsym(RTLD_DEFAULT, "glGetString");
    const unsigned char *r = real ? real(name) : NULL;
    return r ? r : (const unsigned char *)"";
  }
  }
}

/* Roteador p/ funções GL que precisamos controlar (anti stack-smash). */
static void rgl(const char *n, void **slot);
static void store_shader_src(unsigned sh, const char *s, int len);
static void my_glViewport(int, int, int, int);  /* T2: resolução interna */
static void my_glShaderSource(unsigned, int, const char *const *, const int *);
static void my_glCompileShader(unsigned);
static void my_glLinkProgram(unsigned);
static void my_glTexImage3D(unsigned, int, int, int, int, int, int, unsigned,
                            unsigned, const void *);
static void my_glTexStorage3D(unsigned, int, unsigned, int, int, int);
static void my_glCompressedTexImage3D(unsigned, int, unsigned, int, int, int,
                                      int, int, const void *);
static void my_glCompressedTexImage2D(unsigned, int, unsigned, int, int, int,
                                      int, const void *);
static void my_glTexStorage2D(unsigned, int, unsigned, int, int);
static void my_glUniform4fv(int, int, const float *);
static void my_glUniformMatrix4fv(int, int, unsigned char, const float *);
static void my_glUniform3fv(int, int, const float *);
static void my_glUniform1f(int, float);
static void my_glUniform4i(int, int, int, int, int);
static void my_glUniform1i(int, int);
static void my_glUseProgram(unsigned);
static int my_glGetUniformLocation(unsigned, const char *);
void *coi_gl_proc_override(const char *name) {
  if (name && strcmp(name, "glGetString") == 0) return (void *)my_glGetString;
  if (name && strcmp(name, "glShaderSource") == 0) return (void *)my_glShaderSource;
  if (name && strcmp(name, "glCompileShader") == 0) return (void *)my_glCompileShader;
  if (name && strcmp(name, "glLinkProgram") == 0) return (void *)my_glLinkProgram;
  /* GLES3-only (texture arrays/3D): logamos p/ flagrar o uso no Mali GLES2 */
  if (name && strcmp(name, "glTexImage3D") == 0) return (void *)my_glTexImage3D;
  if (name && strcmp(name, "glTexStorage3D") == 0) return (void *)my_glTexStorage3D;
  if (name && strcmp(name, "glCompressedTexImage3D") == 0) return (void *)my_glCompressedTexImage3D;
  if (name && strcmp(name, "glCompressedTexImage2D") == 0) return (void *)my_glCompressedTexImage2D;
  if (name && strcmp(name, "glTexStorage2D") == 0) return (void *)my_glTexStorage2D;
  if (name && strcmp(name, "glViewport") == 0) return (void *)my_glViewport;
  /* COI diag Mickey preto: uniforms de luz podem ser resolvidos via
   * eglGetProcAddress/dlsym (bypass da tabela) — rotear pros wrappers */
  if (name && strcmp(name, "glUniformMatrix4fv") == 0) return (void *)my_glUniformMatrix4fv;
  if (name && strcmp(name, "glUniform4fv") == 0) return (void *)my_glUniform4fv;
  if (name && strcmp(name, "glUniform3fv") == 0) return (void *)my_glUniform3fv;
  if (name && strcmp(name, "glUniform1f") == 0) return (void *)my_glUniform1f;
  if (name && strcmp(name, "glUniform4i") == 0) return (void *)my_glUniform4i;
  if (name && strcmp(name, "glUniform1i") == 0) return (void *)my_glUniform1i;
  if (name && strcmp(name, "glUseProgram") == 0) return (void *)my_glUseProgram;
  if (name && strcmp(name, "glGetUniformLocation") == 0) return (void *)my_glGetUniformLocation;
  return NULL;
}
/* 🧊 ETC2 → RGBA na CPU (GLES2-universal, até Utgard): com FORCE_ETC2 a engine
 * carrega os .ktx (ETC2) — resolve os APKs com JPEG vazio SEM tool no PC.
 * Decodificamos o bloco ETC2 e subimos via my_glTexImage2D (ganha TEXSCALE).
 * Formatos: 0x9274/75 RGB8, 0x9276/77 punchthrough, 0x9278/79 RGBA8. */
extern unsigned char *etc2_decode_rgba(unsigned fmt, int w, int h,
                                       const void *data, int size);
static void my_glTexImage2D(unsigned, int, int, int, int, int, unsigned,
                            unsigned, const void *);
static unsigned g_active_unit;      /* def. real mais abaixo (tracker de bind) */
static unsigned g_bound_tex[8];
static void my_glCompressedTexImage2D(unsigned tgt, int lvl, unsigned ifmt,
                                      int w, int h, int border, int sz,
                                      const void *px) {
  static void (*real)(unsigned, int, unsigned, int, int, int, int,
                      const void *) = NULL;
  rgl("glCompressedTexImage2D", (void **)&real);
  static unsigned (*gerr)(void) = NULL; rgl("glGetError", (void **)&gerr);
  /* 🟢 ETC2 PASSTHROUGH (GPU ES3 que amostra ETC2 nativo: Mali-G31/R36S, etc): sobe
   * os blocos ETC2 ORIGINAIS direto — SEM decode, SEM relabel p/ ETC1 — ~8× menos
   * VRAM e zero CPU. CRUCIAL em device de RAM baixa (R36S 481MB) e o caminho NATIVO
   * do motor (foi feito p/ ETC2-no-ES3). Decodifica só se o driver recusar. */
  if (px && ifmt >= 0x9274 && ifmt <= 0x9279 &&
      getenv("COI_ETC2_PASSTHROUGH")) {
    if (gerr) while (gerr()) {}
    if (real) real(tgt, lvl, ifmt, w, h, border, sz, px);
    unsigned e = gerr ? gerr() : 0;
    static int dn = 0;
    if (dn < 8) { fprintf(stderr, "[ETC2PASS] 0x%x %dx%d lvl=%d err=0x%x\n", ifmt, w, h, lvl, e); dn++; }
    if (!e) return;
    unsigned char *rgba = etc2_decode_rgba(ifmt, w, h, px, sz); /* fallback */
    if (rgba) { my_glTexImage2D(tgt, lvl, 0x1908, w, h, border, 0x1908, 0x1401, rgba); free(rgba); }
    return;
  }
  /* 🔑 ETC1 DIRETO: nosso .ktx opaco = conteúdo ETC1 rotulado ETC2-RGB (0x9274/75). O
   * Mali-450 AMOSTRA ETC1 nativo (0x8D64) -> sobe DIRETO, sem decodificar em runtime
   * (4bpp, zero CPU). Se o GL recusar (não devia), cai pro decode. As 0x9276-0x9279
   * (ETC2 RGBA/punchthrough = alpha) o Mali NÃO amostra -> decodifica p/ RGBA. */
  if (px && (ifmt == 0x9274 || ifmt == 0x9275) && !getenv("COI_ETC1_DECODE")) {
    if (gerr) while (gerr()) {}
    if (real) real(tgt, lvl, 0x8D64, w, h, border, sz, px);
    unsigned e = gerr ? gerr() : 0;
    static int dn = 0;
    if (dn < 8) { fprintf(stderr, "[ETC1DIRECT] 0x%x->0x8D64 %dx%d lvl=%d err=0x%x\n", ifmt, w, h, lvl, e); dn++; }
    if (!e) return;
    /* fallback: decodifica */
    unsigned char *rgba = etc2_decode_rgba(0x9274, w, h, px, sz);
    if (rgba) { my_glTexImage2D(tgt, lvl, 0x1908, w, h, border, 0x1908, 0x1401, rgba); free(rgba); }
    return;
  }
  /* As texturas que a engine carrega COMPRIMIDAS (ETC2) são DECODIFICADAS p/ RGBA
   * (Mali-450 não amostra ETC2). NÃO subir como ETC1 cru (modos planar/T/H do ETC2
   * viram lixo/magenta no chão liso). Caminho validado da v5. */
  if (px && ifmt >= 0x9274 && ifmt <= 0x9279) {
    unsigned char *rgba = etc2_decode_rgba(ifmt, w, h, px, sz);
    if (rgba) {
      my_glTexImage2D(tgt, lvl, 0x1908 /*RGBA*/, w, h, border,
                      0x1908, 0x1401 /*UBYTE*/, rgba);
      free(rgba);
      static int dn = 0;
      if (dn < 8) { fprintf(stderr, "[ETC2] decode 0x%x %dx%d lvl=%d -> RGBA\n",
                            ifmt, w, h, lvl); dn++; }
      return;
    }
  }
  if (gerr) while (gerr()) {}
  if (real) real(tgt, lvl, ifmt, w, h, border, sz, px);
  unsigned e = gerr ? gerr() : 0;
  static int n = 0;
  if (n < 40 || e) {
    fprintf(stderr, "[CTEX] glCompressedTexImage2D ifmt=0x%x %dx%d sz=%d lvl=%d -> err=0x%x\n",
            ifmt, w, h, sz, lvl, e);
    n++;
  }
  /* COI_TEXID: rastreia TODO upload comprimido com o id bindado (correlacionar
   * com o tex0 do draw do personagem) */
  if (getenv("COI_TEXID"))
    fprintf(stderr, "[TEXID] C id=%u lvl=%d ifmt=0x%x %dx%d sz=%d err=0x%x\n",
            g_bound_tex[g_active_unit < 8 ? g_active_unit : 0], lvl, ifmt, w, h, sz, e);
  /* COI_TEXDUMP: salva payload ETC1 lvl0 256x256 p/ decodificar no host */
  if (px && lvl == 0 && w == 256 && h == 256 && getenv("COI_TEXDUMP")) {
    static int td = 0;
    if (td < 400) {
      char nm[64];
      snprintf(nm, sizeof(nm), "texetc_%u.bin",
               g_bound_tex[g_active_unit < 8 ? g_active_unit : 0]);
      FILE *tf = fopen(nm, "wb");
      if (tf) { fwrite(px, 1, (size_t)sz, tf); fclose(tf); td++; }
    }
  }
}
static void my_glTexStorage2D(unsigned tgt, int lvls, unsigned ifmt, int w,
                              int h) {
  static void (*real)(unsigned, int, unsigned, int, int) = NULL;
  rgl("glTexStorage2D", (void **)&real);
  static int n = 0;
  if (n < 40) { fprintf(stderr, "[TEXSTOR] glTexStorage2D ifmt=0x%x %dx%d lvls=%d\n", ifmt, w, h, lvls); n++; }
  if (real) real(tgt, lvls, ifmt, w, h);
}
/* glTexImage2D: loga ifmt/fmt/type/dim + erro GL. ifmt GLES3 (ex: GL_RGBA8
 * 0x8058, GL_SRGB8 0x8C41, sized) NÃO existe no GLES2 → INVALID_ENUM →
 * textura branca. GLES2 quer ifmt = base (GL_RGBA 0x1908) sem sized. */
/* ===== AUTO-FIX do mismatch stride buffer(0x7F=40) × atributos(0x7=24) =====
 * O render às vezes liga um buffer de formato MAIOR (variante 0x7F/40B, pq a
 * variante pedida falhou de criar) mas configura atributos com stride do
 * formato pedido (0x7/24B). A GPU lê vértices errados → branco. Rastreamos o
 * stride REAL de cada GL buffer (via formato do createvb) e, no
 * glVertexAttribPointer, se o buffer ligado tem stride maior, usamos o dele
 * (os offsets pos@0/cor@12/uv@16 valem nos dois formatos). */
static unsigned g_cur_array_buf = 0;
typedef struct {
  int en, size, norm, stride;
  unsigned type, buf;
  long off;
} CoiAttr;
static CoiAttr g_coi_attr[16];
/* estado GL rastreado p/ [DRAWATTR] (diag passes do Mickey) */
static unsigned g_st_depthfunc = 0x0201, g_st_bsrc = 1, g_st_bdst = 0;
static int g_st_depthmask = 1, g_st_blend = 0, g_st_depth = 0, g_st_cull = 0,
           g_st_stencil = 0, g_st_cmask = 0xf;
static int g_buf_stride[4096];        /* id -> stride real do buffer */
static long g_buf_size[4096];         /* id -> tamanho do buffer (p/ divisibilidade) */

/* Action Squad's D3D-style renderer always submits 32-bit element indices,
 * including its six-index level-composition quads.  This Mali-450 MP driver
 * does not expose GL_OES_element_index_uint and rejects those draws with
 * GL_INVALID_ENUM.  Retain a CPU copy of each element buffer so the draw
 * wrapper can present a lossless uint16 view when every referenced index fits.
 * The original GL buffer and the engine's data remain untouched. */
#define AS_GL_TRACKED_BUFFERS 4096
typedef struct {
  unsigned char *data;
  size_t size;
  size_t capacity;
  unsigned mirror_id;
  int mirror_valid;
} AsElementBufferCopy;
static AsElementBufferCopy g_as_element_buffers[AS_GL_TRACKED_BUFFERS];
static unsigned g_cur_element_buf = 0;
static unsigned short *g_as_u16_indices = NULL;
static size_t g_as_u16_capacity = 0;

static int as_gl_has_extension(const char *extensions, const char *wanted) {
  if (!extensions || !wanted || !*wanted) return 0;
  size_t n = strlen(wanted);
  const char *p = extensions;
  while ((p = strstr(p, wanted)) != NULL) {
    if ((p == extensions || p[-1] == ' ') &&
        (p[n] == '\0' || p[n] == ' ')) return 1;
    p += n;
  }
  return 0;
}

static int as_gl_supports_uint_indices(void) {
  static int supported = -1;
  if (supported >= 0) return supported;
  const unsigned char *(*real_get_string)(unsigned) = NULL;
  rgl("glGetString", (void **)&real_get_string);
  const char *version = real_get_string
      ? (const char *)real_get_string(0x1F02 /* GL_VERSION */) : NULL;
  const char *extensions = real_get_string
      ? (const char *)real_get_string(0x1F03 /* GL_EXTENSIONS */) : NULL;
  supported = (version && (strstr(version, "OpenGL ES 3.") ||
                            strstr(version, "OpenGL ES 4."))) ||
              as_gl_has_extension(extensions, "GL_OES_element_index_uint");
  fprintf(stderr, "[INDEX16] native uint indices: %s\n",
          supported ? "supported" : "unsupported; lossless uint16 bridge active");
  return supported;
}
/* tabela (tamanho do buffer de vértice -> stride), preenchida pelo createvb
 * (sabe formato→stride e count). O upload glBufferData casa por tamanho. */
static struct { long size; int stride; } g_szstride[512];
static int g_szs_n = 0;
void coi_record_vbsize(long size, int stride) {
  if (size <= 0 || stride <= 0) return;
  g_szstride[g_szs_n % 512].size = size;
  g_szstride[g_szs_n % 512].stride = stride;
  g_szs_n++;
}
static int lookup_stride_by_size(long size) {
  int lim = g_szs_n < 512 ? g_szs_n : 512;
  for (int i = 0; i < lim; i++)
    if (g_szstride[i].size == size) return g_szstride[i].stride;
  return 0;
}
static int g_stridefix = -1;
static void my_glBindBuffer(unsigned tgt, unsigned id) {
  static void (*real)(unsigned, unsigned) = NULL; rgl("glBindBuffer", (void **)&real);
  if (tgt == 0x8892) g_cur_array_buf = id; /* GL_ARRAY_BUFFER */
  if (tgt == 0x8893) g_cur_element_buf = id; /* GL_ELEMENT_ARRAY_BUFFER */
  if (real) real(tgt, id);
}

/* trace de binding de textura no momento do draw */
static unsigned g_active_unit = 0, g_bound_tex[8] = {0};
static void my_glActiveTexture(unsigned tex) {
  static void (*real)(unsigned) = NULL; rgl("glActiveTexture", (void **)&real);
  g_active_unit = tex - 0x84C0; /* GL_TEXTURE0 */
  if (g_active_unit >= 8) g_active_unit = 0;
  if (real) real(tex);
}
static void coip_on_bind(unsigned target, unsigned id); /* fwd (DYS_PAGE, abaixo) */
static void my_glBindTexture(unsigned tgt, unsigned tex) {
  static void (*real)(unsigned, unsigned) = NULL; rgl("glBindTexture", (void **)&real);
  if (g_active_unit < 8) g_bound_tex[g_active_unit] = tex;
  if (real) real(tgt, tex);
  coip_on_bind(tgt, tex);
}

/* ===================== DYS_PAGE: paginacao/streaming de textura (porte do Bully) =============
 * O motor 10tons nunca despeja textura; na UMA do Mali textura GL = RAM. Com COI_PAGE=1,
 * cada textura subida com pixels reais vira "pageavel": os pixels vao pro SD no upload
 * (<swapdir>/<id>.tx, ID-KEYED = imune a atlas/nome-stale); acima do orcamento despejamos a
 * mais FRIA (re-define 1x1 -> libera RAM, id continua valido); no re-bind de uma despejada
 * (page fault) re-subimos do SD -- sincrono, ou assincrono via worker (pop-in, sem freeze).
 * Gates: COI_PAGE=1 + COI_PAGE_SWAP=<dir> (obrigatorios), COI_PAGE_CAP_MB
 * (default 200), COI_PAGE_ASYNC=1, COI_PAGELOG=1.
 * DIFERENCA vs Bully: a engine ATLASA em runtime (glTexSubImage2D) -> textura que recebe
 * SubImage/CopySub vira NAO-pageavel (MVP §2.3 do ESTUDO-PORTMASTER-1GB-STREAMING.md).
 * O npot_fix global permanece OFF por padrão; paging só existe quando seus
 * próprios gates opt-in estão ativos. */
#define DYSP_MAX 262144
static unsigned       g_coip_bytes[DYSP_MAX];
static unsigned char  g_coip_kind[DYSP_MAX];      /* 0=nao-pageavel, 2=SWAP */
static unsigned short g_coip_w[DYSP_MAX], g_coip_h[DYSP_MAX];
static unsigned char  g_coip_present[DYSP_MAX];
static unsigned char  g_coip_req[DYSP_MAX];       /* pedido em voo (async) */
static unsigned       g_coip_use[DYSP_MAX];
static unsigned       g_coip_clock = 0;
static long long      g_coip_resident = 0;
static unsigned       g_coip_list[65536]; static int g_coip_n = 0;
static long g_coip_pf = 0, g_coip_ev = 0, g_coip_sub = 0;
struct coip_swap_hdr { unsigned magic; int w, h, ifmt; unsigned ufmt, utype, bytes; };
#define DYSP_MAGIC 0xD75A9E11u
static const char *coip_swapdir(void){ static const char*d; static int got; if(!got){ d=getenv("COI_PAGE_SWAP"); got=1; } return d; }
static int coi_paging(void){ static int m=-1; if(m<0)m=(getenv("COI_PAGE")&&coip_swapdir())?1:0; return m; }
static long long coip_cap(void){ static long long c=-1; if(c<0){ const char*e=getenv("COI_PAGE_CAP_MB"); c=(long long)(e?atoll(e):200)*1024*1024; } return c; }
static int coip_async(void){ static int m=-1; if(m<0)m=getenv("COI_PAGE_ASYNC")?1:0; return m; }
/* piso de MemAvailable (rede anti-OOM, do texpage do Bully): se a RAM LIVRE do sistema cair
 * abaixo do piso, despeja frias MESMO abaixo do cap (crucial em device SEM swap, ex R36S). */
static long long coip_floor(void){ static long long f=-1; if(f<0){ const char*e=getenv("COI_PAGE_FLOOR_MB"); f=(long long)(e?atoll(e):0)*1024*1024; } return f; }
static long long g_coip_swapfree = -1, g_coip_swaptotal = -1; /* bytes (da ultima leitura) */
static long long coip_mem_avail(void){
  FILE *f = fopen("/proc/meminfo", "r"); if (!f) return -1;
  char ln[160]; long kb = -1, sf = -1, st = -1;
  while (fgets(ln, sizeof ln, f)) {
    if (!strncmp(ln, "MemAvailable:", 13)) kb = atol(ln + 13);
    else if (!strncmp(ln, "SwapTotal:", 10)) st = atol(ln + 10);
    else if (!strncmp(ln, "SwapFree:", 9)) sf = atol(ln + 9);
  }
  fclose(f);
  g_coip_swaptotal = st >= 0 ? (long long)st * 1024 : -1;
  g_coip_swapfree  = sf >= 0 ? (long long)sf * 1024 : -1;
  return kb >= 0 ? (long long)kb * 1024 : -1;
}
static int coip_bpp(unsigned fmt, unsigned typ) {
  if (typ == 0x8033 /*4444*/ || typ == 0x8034 /*5551*/ || typ == 0x8363 /*565*/) return 2;
  if (typ != 0x1401 /*UBYTE*/) return 0;
  switch (fmt) { case 0x1908: return 4; case 0x1907: return 3; case 0x190A: return 2;
                 case 0x1909: case 0x1906: return 1; default: return 0; }
}
/* tira a textura do sistema (RT/atlas dinamico/delete) e apaga o swap dela */
static void coip_unpage(unsigned id) {
  if (id >= DYSP_MAX || !g_coip_kind[id]) return;
  if (g_coip_present[id]) g_coip_resident -= g_coip_bytes[id];
  g_coip_kind[id] = 0; g_coip_present[id] = 0; g_coip_bytes[id] = 0;
  if (coip_swapdir()) { char p[320]; snprintf(p, sizeof p, "%s/%u.tx", coip_swapdir(), id); remove(p); }
}
/* upload com pixels reais: grava/atualiza o swap (redefinicao = conteudo novo) e contabiliza */
static unsigned coip_min_bytes(void) {
  static long v = -1;
  if (v < 0) { const char *e = getenv("COI_PAGE_MIN_KB"); v = (e ? atol(e) : 96) * 1024; }
  return (unsigned)v;
}
static void coip_note_upload(unsigned id, int ifmt, int w, int h, unsigned ufmt, unsigned utype,
                             const void *data, unsigned bytes) {
  if (!coi_paging() || id == 0 || id >= DYSP_MAX || !data || !bytes) return;
  if (bytes < coip_min_bytes()) return;        /* pequenas nao valem paginar */
  char path[320]; snprintf(path, sizeof path, "%s/%u.tx", coip_swapdir(), id);
  FILE *f = fopen(path, "wb"); if (!f) return;
  struct coip_swap_hdr hd = { DYSP_MAGIC, w, h, ifmt, ufmt, utype, bytes };
  fwrite(&hd, sizeof hd, 1, f); fwrite(data, 1, bytes, f);
  /* solta o page-cache do swap (licao Bully r6: cache do texswap enchia a RAM e o
   * kernel swapava o JOGO; DONTNEED torna reclaimavel na hora) */
  fflush(f); posix_fadvise(fileno(f), 0, 0, POSIX_FADV_DONTNEED); fclose(f);
  if (!g_coip_kind[id] && g_coip_n < 65536) g_coip_list[g_coip_n++] = id;
  int was = g_coip_kind[id] && g_coip_present[id];
  g_coip_resident += was ? (long long)bytes - g_coip_bytes[id] : (long long)bytes;
  g_coip_kind[id] = 2; g_coip_present[id] = 1; g_coip_bytes[id] = bytes;
  g_coip_w[id] = (unsigned short)w; g_coip_h[id] = (unsigned short)h; g_coip_use[id] = ++g_coip_clock;
}
/* despeja as mais FRIAS ate voltar ao orcamento 'capb' (pula a atual e as ja-despejadas) */
static void coip_evict_to(unsigned target, unsigned keep, long long capb, unsigned min_age) {
  static void (*rTexImg)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*) = NULL;
  static void (*rBind)(unsigned,unsigned) = NULL;
  if (!rTexImg) rTexImg = dlsym(RTLD_DEFAULT, "glTexImage2D");
  if (!rBind)   rBind   = dlsym(RTLD_DEFAULT, "glBindTexture");
  if (!rTexImg || !rBind) return;
  static const unsigned char black[4] = {0,0,0,0};
  int guard = 0;
  while (g_coip_resident > capb && guard++ < 4096) {
    unsigned best = 0, bu = 0xffffffffu; int fi = -1;
    for (int i = 0; i < g_coip_n; i++) { unsigned id = g_coip_list[i];
      if (id == keep || g_coip_kind[id] != 2 || !g_coip_present[id]) continue;
      if (min_age && g_coip_clock - g_coip_use[id] < min_age) continue; /* FRIA apenas (anti-piscada) */
      if (g_coip_use[id] < bu) { bu = g_coip_use[id]; best = id; fi = i; } }
    if (fi < 0) break;
    long long b = g_coip_bytes[best];
    rBind(0x0DE1, best);
    rTexImg(0x0DE1, 0, 0x1907, 1, 1, 0, 0x1907, 0x1401, black);  /* 1x1 -> libera a grande */
    g_coip_resident -= b; g_coip_bytes[best] = 3; g_coip_present[best] = 0; g_coip_ev++;
  }
  rBind(target, keep);
}
struct coip_ready { unsigned id; unsigned char *buf; int w, h, ifmt; unsigned ufmt, utype, bytes; };
#define DYSP_RING 4096
static unsigned g_coip_rring[DYSP_RING]; static int g_coip_rh = 0, g_coip_rt = 0;
static struct coip_ready g_coip_ready_l[DYSP_RING]; static int g_coip_ready_n = 0;
static struct coip_ready g_coip_drain_l[DYSP_RING];
static pthread_mutex_t g_coip_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_coip_cv  = PTHREAD_COND_INITIALIZER;
static pthread_t g_coip_thr; static int g_coip_thr_on = 0;
/* SO I/O (pode rodar no worker) */
static int coip_read(unsigned id, struct coip_ready *out) {
  char path[320]; snprintf(path, sizeof path, "%s/%u.tx", coip_swapdir(), id);
  FILE *f = fopen(path, "rb"); if (!f) return 0;
  struct coip_swap_hdr hd;
  if (fread(&hd, sizeof hd, 1, f) != 1 || hd.magic != DYSP_MAGIC) { fclose(f); return 0; }
  unsigned char *buf = malloc(hd.bytes); size_t r = buf ? fread(buf, 1, hd.bytes, f) : 0;
  posix_fadvise(fileno(f), 0, 0, POSIX_FADV_DONTNEED);  /* nao poluir o page-cache */
  fclose(f);
  if (!buf || r != hd.bytes) { free(buf); return 0; }
  out->id = id; out->buf = buf; out->w = hd.w; out->h = hd.h; out->ifmt = hd.ifmt;
  out->ufmt = hd.ufmt; out->utype = hd.utype; out->bytes = hd.bytes;
  return 1;
}
/* SO GL (render thread): deixa o id BINDADO ao final */
static void coip_upload(const struct coip_ready *pr) {
  static void (*rTexImg)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*) = NULL;
  static void (*rParam)(unsigned,unsigned,int) = NULL;
  static void (*rBind)(unsigned,unsigned) = NULL;
  if (!rTexImg) rTexImg = dlsym(RTLD_DEFAULT, "glTexImage2D");
  if (!rParam)  rParam  = dlsym(RTLD_DEFAULT, "glTexParameteri");
  if (!rBind)   rBind   = dlsym(RTLD_DEFAULT, "glBindTexture");
  if (!rTexImg || !rBind) return;
  rBind(0x0DE1, pr->id);
  rTexImg(0x0DE1, 0, pr->ifmt, pr->w, pr->h, 0, pr->ufmt, pr->utype, pr->buf);
  if (rParam) { rParam(0x0DE1, 0x2801, 0x2601); rParam(0x0DE1, 0x2800, 0x2601); } /* LINEAR */
  g_coip_resident += pr->bytes; g_coip_bytes[pr->id] = pr->bytes; g_coip_present[pr->id] = 1; g_coip_pf++;
}
static void coip_fault(unsigned id) {
  struct coip_ready pr; if (coip_read(id, &pr)) { coip_upload(&pr); free(pr.buf); }
}
static void *coip_worker(void *arg) {
  (void)arg;
  /* prioridade levemente reduzida: I/O de fundo nao compete com render/audio */
  setpriority(PRIO_PROCESS, (id_t)syscall(178), 5);
  for (;;) {
    unsigned id;
    pthread_mutex_lock(&g_coip_mtx);
    while (g_coip_rh == g_coip_rt) pthread_cond_wait(&g_coip_cv, &g_coip_mtx);
    id = g_coip_rring[g_coip_rh]; g_coip_rh = (g_coip_rh + 1) % DYSP_RING;
    pthread_mutex_unlock(&g_coip_mtx);
    struct coip_ready pr;
    if (coip_read(id, &pr)) {
      pthread_mutex_lock(&g_coip_mtx);
      if (g_coip_ready_n < DYSP_RING) g_coip_ready_l[g_coip_ready_n++] = pr; else free(pr.buf);
      pthread_mutex_unlock(&g_coip_mtx);
    } else {
      pthread_mutex_lock(&g_coip_mtx);
      if (id < DYSP_MAX) g_coip_req[id] = 0;
      pthread_mutex_unlock(&g_coip_mtx);
    }
  }
  return NULL;
}
static void coip_enqueue(unsigned id) {
  pthread_mutex_lock(&g_coip_mtx);
  if (!g_coip_req[id]) {
    int nt = (g_coip_rt + 1) % DYSP_RING;
    if (nt != g_coip_rh) { g_coip_rring[g_coip_rt] = id; g_coip_rt = nt; g_coip_req[id] = 1;
      pthread_cond_signal(&g_coip_cv); }
  }
  pthread_mutex_unlock(&g_coip_mtx);
}
static void coip_drain(unsigned restore) {
  int n;
  pthread_mutex_lock(&g_coip_mtx);
  n = g_coip_ready_n; for (int i = 0; i < n; i++) g_coip_drain_l[i] = g_coip_ready_l[i]; g_coip_ready_n = 0;
  pthread_mutex_unlock(&g_coip_mtx);
  if (!n) return;
  for (int i = 0; i < n; i++) { struct coip_ready *pr = &g_coip_drain_l[i];
    if (pr->id < DYSP_MAX && g_coip_kind[pr->id] == 2 && !g_coip_present[pr->id]) coip_upload(pr);
    free(pr->buf); }
  pthread_mutex_lock(&g_coip_mtx);
  for (int i = 0; i < n; i++) if (g_coip_drain_l[i].id < DYSP_MAX) g_coip_req[g_coip_drain_l[i].id] = 0;
  pthread_mutex_unlock(&g_coip_mtx);
  static void (*rBind)(unsigned,unsigned) = NULL;
  if (!rBind) rBind = dlsym(RTLD_DEFAULT, "glBindTexture");
  if (rBind) rBind(0x0DE1, restore);
}
static void coip_on_bind(unsigned target, unsigned id) {
  if (!coi_paging() || target != 0x0DE1 || id >= DYSP_MAX || g_coip_kind[id] != 2) return;
  g_coip_use[id] = ++g_coip_clock;
  if (coip_async()) {
    if (g_coip_thr_on == 0) g_coip_thr_on = (pthread_create(&g_coip_thr, NULL, coip_worker, NULL) == 0) ? 1 : -1;
    if (g_coip_thr_on == 1) {
      if (!g_coip_present[id]) coip_enqueue(id);   /* pop-in: 1x1 ate o worker ler */
      coip_drain(id);
    } else if (!g_coip_present[id]) coip_fault(id);
  } else if (!g_coip_present[id]) coip_fault(id);
  if (g_coip_resident > coip_cap()) coip_evict_to(target, id, coip_cap(), 0);
  /* piso anti-OOM: checa MemAvailable a cada ~256 binds; abaixo do piso, encolhe o
   * residente (8MB por checagem; CRITICO <piso/2 = -25%) mesmo abaixo do cap.
   * SO despeja textura FRIA (nao usada ha >=6000 binds) e nunca abaixo de 8MB —
   * senao despeja textura VISIVEL = piscada preta (visto no R36S 639MB). */
  if (coip_floor() > 0) {
    static unsigned fc = 0;
    if ((++fc & 0xFF) == 0) {
      long long avail = coip_mem_avail();
      long long keepmin = (long long)8 * 1024 * 1024;
      /* zram/swap SATURADO = pressao real mesmo com MemAvailable "ok" (OOM do .160:
       * zram 193/255 cheio, avail ~50MB, floor nao disparava e o kernel matou o jogo) */
      int swap_low  = (g_coip_swaptotal > 0 && g_coip_swapfree >= 0 &&
                       g_coip_swapfree < g_coip_swaptotal / 8);   /* <12.5% livre */
      int swap_crit = (g_coip_swaptotal > 0 && g_coip_swapfree >= 0 &&
                       g_coip_swapfree < g_coip_swaptotal / 16);  /* <6% livre */
      if (avail >= 0 && (avail < coip_floor() || swap_low) && g_coip_resident > keepmin) {
        /* CRITICO (avail < piso/2): despeja IGNORANDO idade — pop-in momentaneo e
         * melhor que OOM-kill (visto no R36S .160: em pico de carga nada e "frio",
         * o cold-guard travava o floor e o kernel matava o jogo). Normal: so FRIAS. */
        int critical = (avail < coip_floor() / 2) || swap_crit;
        /* pool ja no minimo = despejar visivel NAO ajuda em nada (a pressao e do
         * MUNDO, nao das texturas) -> vira so PISCA-PISCA infinito (visto .160,
         * zram 255/255 + resident 7MB). Nesse caso NAO faz sweep critico. */
        if (critical && g_coip_resident <= keepmin * 3) critical = 0;
        /* cooldown do critico: no maximo 1 sweep agressivo a cada ~8 checagens
         * (~2000 binds) — pisca RARO em vez de constante */
        static unsigned last_crit = 0; static unsigned checks = 0; checks++;
        if (critical && checks - last_crit < 8) critical = 0; else if (critical) last_crit = checks;
        long long cut = critical ? g_coip_resident / 2 : (long long)8 * 1024 * 1024;
        long long tgt2 = g_coip_resident - cut; if (tgt2 < keepmin) tgt2 = keepmin;
        coip_evict_to(target, id, tgt2, critical ? 0 : 6000);
        if (getenv("COI_PAGELOG"))
          fprintf(stderr, "[coip] PRESSAO%s avail=%lldMB piso=%lldMB -> evict p/ %lldMB\n",
                  critical ? " CRITICA" : "", avail/(1024*1024), coip_floor()/(1024*1024),
                  tgt2/(1024*1024));
      }
    }
  }
  if (getenv("COI_PAGELOG")) { static long c = 0; if ((c++ % 600) == 0)
    fprintf(stderr, "[coip] resident=%lldMB cap=%lldMB pf=%ld ev=%ld sub=%ld pageaveis=%d ready=%d\n",
            g_coip_resident/(1024*1024), coip_cap()/(1024*1024), g_coip_pf, g_coip_ev, g_coip_sub,
            g_coip_n, g_coip_ready_n); }
}
/* atlas dinamico: garante conteudo, marca nao-pageavel (MVP) e passa adiante */
static void my_glTexSubImage2D(unsigned tgt, int lvl, int xo, int yo, int w, int h,
                               unsigned fmt, unsigned typ, const void *px) {
  static void (*real)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*) = NULL;
  rgl("glTexSubImage2D", (void **)&real);
  if (coi_paging() && tgt == 0x0DE1) {
    unsigned id = g_bound_tex[g_active_unit < 8 ? g_active_unit : 0];
    if (id < DYSP_MAX && g_coip_kind[id] == 2) {
      if (!g_coip_present[id]) coip_fault(id);
      g_coip_sub++; coip_unpage(id);
    }
  }
  if (real) real(tgt, lvl, xo, yo, w, h, fmt, typ, px);
}
static void my_glCopyTexSubImage2D(unsigned tgt, int lvl, int xo, int yo, int x, int y, int w, int h) {
  static void (*real)(unsigned,int,int,int,int,int,int,int) = NULL;
  rgl("glCopyTexSubImage2D", (void **)&real);
  if (coi_paging() && tgt == 0x0DE1) {
    unsigned id = g_bound_tex[g_active_unit < 8 ? g_active_unit : 0];
    if (id < DYSP_MAX && g_coip_kind[id] == 2) {
      if (!g_coip_present[id]) coip_fault(id);
      g_coip_sub++; coip_unpage(id);
    }
  }
  if (real) real(tgt, lvl, xo, yo, x, y, w, h);
}
static void my_glDeleteTextures(int n, const unsigned *ids) {
  static void (*real)(int, const unsigned *) = NULL;
  rgl("glDeleteTextures", (void **)&real);
  if (coi_paging() && ids) for (int i = 0; i < n; i++) if (ids[i] < DYSP_MAX) coip_unpage(ids[i]);
  if (real) real(n, ids);
}

/* gate do UNIF_LOG: com COI_UNIF_GATE=1 so loga enquanto
 * /dev/shm/coi_uniflog existir (liga na gameplay, caps preservados) */
static int unif_log_on(void) {
  if (!getenv("COI_UNIF_LOG")) return 0;
  if (getenv("COI_UNIF_GATE") && access("/dev/shm/coi_uniflog", F_OK) != 0)
    return 0;
  return 1;
}
static void my_glUniform1i(int loc, int v) {
  static void (*real)(int, int) = NULL; rgl("glUniform1i", (void **)&real);
  static int n = 0;
  if (unif_log_on() && n < 50) {
    fprintf(stderr, "[UNIF1i] loc=%d val=%d\n", loc, v); n++;
  }
  if (real) real(loc, v);
}
static int my_glGetUniformLocation(unsigned prog, const char *nm) {
  static int (*real)(unsigned, const char *) = NULL;
  rgl("glGetUniformLocation", (void **)&real);
  int r = real ? real(prog, nm) : -1;
  static int n = 0;
  if (getenv("COI_UNIF_LOG") && n < 2500 && nm) { /* sem gate: locs vem no link, pre-gameplay */
    fprintf(stderr, "[UNIFLOC] %s -> loc %d (prog %u)\n", nm, r, prog); n++;
  }
  return r;
}
/* COI diag Mickey preto: valores dos uniforms vetoriais (luz do personagem).
 * COI_UNIF_LOG=1 -> loga primeiros N glUniform3fv/4fv com prog/loc/v */
static unsigned g_cur_prog; /* fwd (setado em my_glUseProgram abaixo) */
static void my_glUniform4fv(int loc, int cnt, const float *v) {
  static void (*real)(int, int, const float *) = NULL;
  rgl("glUniform4fv", (void **)&real);
  static int n = 0;
  if (unif_log_on() && n < 300 && v) {
    fprintf(stderr, "[UNIF4fv] prog=%u loc=%d cnt=%d v0=%.3f,%.3f,%.3f,%.3f\n",
            g_cur_prog, loc, cnt, v[0], v[1], v[2], v[3]); n++;
  }
  if (real) real(loc, cnt, v);
}
static void my_glUniform3fv(int loc, int cnt, const float *v) {
  static void (*real)(int, int, const float *) = NULL;
  rgl("glUniform3fv", (void **)&real);
  static int n = 0;
  if (unif_log_on() && n < 300 && v) {
    fprintf(stderr, "[UNIF3fv] prog=%u loc=%d cnt=%d v0=%.3f,%.3f,%.3f\n",
            g_cur_prog, loc, cnt, v[0], v[1], v[2]); n++;
  }
  if (real) real(loc, cnt, v);
}
static void my_glUniform1f(int loc, float v) {
  static void (*real)(int, float) = NULL;
  rgl("glUniform1f", (void **)&real);
  static int n = 0;
  if (unif_log_on() && n < 200) {
    fprintf(stderr, "[UNIF1f] prog=%u loc=%d v=%.3f\n", g_cur_prog, loc, v); n++;
  }
  if (real) real(loc, v);
}
static void my_glUniform4i(int loc, int a, int b, int c, int d) {
  static void (*real)(int, int, int, int, int) = NULL;
  rgl("glUniform4i", (void **)&real);
  static int n = 0;
  if (unif_log_on() && n < 200) {
    fprintf(stderr, "[UNIF4i] prog=%u loc=%d %d,%d,%d,%d\n", g_cur_prog, loc, a, b, c, d); n++;
  }
  if (real) real(loc, a, b, c, d);
}
static void my_glUniformMatrix4fv(int loc, int cnt, unsigned char transpose,
                                  const float *v) {
  static void (*real)(int, int, unsigned char, const float *) = NULL;
  rgl("glUniformMatrix4fv", (void **)&real);
  static int n = 0;
  if (unif_log_on() && n < 600 && v) {
    fprintf(stderr, "[UNIFM4] prog=%u loc=%d cnt=%d t=%d v0=%.3f,%.3f,%.3f,%.3f\n",
            g_cur_prog, loc, cnt, transpose, v[0], v[1], v[2], v[3]); n++;
  }
  if (real) real(loc, cnt, transpose, v);
}
static void my_glUseProgram(unsigned p) {
  static void (*real)(unsigned) = NULL; rgl("glUseProgram", (void **)&real);
  g_cur_prog = p;
  if (real) real(p);
}
static void my_glBindAttribLocation(unsigned prog, unsigned idx, const char *nm) {
  static void (*real)(unsigned, unsigned, const char *) = NULL;
  rgl("glBindAttribLocation", (void **)&real);
  static int n = 0;
  if (getenv("COI_ATTR_LOG") && n < 30) {
    fprintf(stderr, "[BINDATTR] %s -> loc %u\n", nm ? nm : "?", idx); n++;
  }
  if (real) real(prog, idx, nm);
}

/* trace de atributos de vértice (UVs erradas → textura amostra canto = branco) */
static void my_glVertexAttribPointer(unsigned idx, int sz, unsigned typ,
                                     unsigned char norm, int stride, const void *ptr) {
  static void (*real)(unsigned, int, unsigned, unsigned char, int, const void *) = NULL;
  rgl("glVertexAttribPointer", (void **)&real);
  if (idx < 16) { /* rastro p/ [DRAWATTR] */
    g_coi_attr[idx].size = sz; g_coi_attr[idx].type = typ;
    g_coi_attr[idx].norm = norm; g_coi_attr[idx].stride = stride;
    g_coi_attr[idx].off = (long)(uintptr_t)ptr; g_coi_attr[idx].buf = g_cur_array_buf;
  }
  static int n = 0;
  if (getenv("COI_ATTR_LOG") && n < 40) {
    fprintf(stderr, "[ATTR] idx=%u size=%d type=0x%x norm=%d stride=%d off=%ld\n",
            idx, sz, typ, norm, stride, (long)(uintptr_t)ptr); n++;
  }
  /* loga o atributo de COR (ubyte normalizado) p/ cada stride distinto */
  if (getenv("COI_ATTR_LOG") && typ == 0x1401 && norm) {
    static int seen[128]; static int sn = 0; int known = 0;
    for (int i = 0; i < sn; i++) if (seen[i] == stride) { known = 1; break; }
    if (!known && sn < 128) { seen[sn++] = stride;
      fprintf(stderr, "[ATTRCOL] stride=%d cor_off=%ld size=%d\n", stride, (long)(uintptr_t)ptr, sz); }
  }
  /* AUTO-FIX por DIVISIBILIDADE: o stride real SEMPRE divide o tamanho do
   * buffer. Se o stride do atributo (ex 24) NÃO divide o tamanho mas existe um
   * stride de vértice conhecido MAIOR que divide (ex 40), usa esse. Os offsets
   * pos@0/cor@12/uv@16 batem (24 primeiros bytes do 0x7F == 0x7). */
  /* OFF por default: o chão usa stride 40 CORRETO (red herring); o fix só
   * arriscava quebrar sprites. COI_STRIDEFIX=1 p/ reativar/experimentar. */
  if (g_stridefix < 0) g_stridefix = getenv("COI_STRIDEFIX") ? 1 : 0;
  if (getenv("COI_STRIDE_DBG")) {
    static int d = 0;
    if (d < 30 && stride >= 12 && g_cur_array_buf < 4096 && g_buf_size[g_cur_array_buf] > 50000) {
      long sb = g_cur_array_buf < 4096 ? g_buf_size[g_cur_array_buf] : -1;
      fprintf(stderr, "[STRIDEDBG] buf=%u size=%ld attr_stride=%d size%%stride=%ld\n",
              g_cur_array_buf, sb, stride, sb > 0 ? sb % stride : -1); d++;
    }
  }
  if (g_stridefix && stride > 0 && g_cur_array_buf < 4096) {
    long sz_buf = g_buf_size[g_cur_array_buf];
    if (sz_buf > 0 && (sz_buf % stride) != 0) {
      const int cands[] = {28,32,36,40,44,48,52,56,60};
      for (unsigned c = 0; c < sizeof(cands)/sizeof(cands[0]); c++) {
        int S = cands[c];
        if (S > stride && (sz_buf % S) == 0 && (long)(uintptr_t)ptr + sz*4 <= S) {
          static int fl = 0;
          if (fl < 10) { fprintf(stderr, "[STRIDEFIX] buf=%u size=%ld attr=%d -> %d (idx=%u off=%ld)\n",
                                 g_cur_array_buf, sz_buf, stride, S, idx, (long)(uintptr_t)ptr); fl++; }
          stride = S; break;
        }
      }
    }
  }
  if (real) real(idx, sz, typ, norm, stride, ptr);
}
static int my_glGetAttribLocation(unsigned prog, const char *name) {
  static int (*real)(unsigned, const char *) = NULL;
  rgl("glGetAttribLocation", (void **)&real);
  int r = real ? real(prog, name) : -1;
  static int n = 0;
  if (getenv("COI_ATTR_LOG") && n < 40) {
    fprintf(stderr, "[ATTRLOC] %s -> %d\n", name ? name : "?", r); n++;
  }
  return r;
}

/* ---- FBO diag (render-to-texture do mundo: incompleto no Mali → branco) ---- */
static unsigned g_cur_fbo = 0;
static unsigned long g_draws_fbo = 0, g_draws_screen = 0;
static void my_glBindFramebuffer(unsigned tgt, unsigned fb) {
  static void (*real)(unsigned, unsigned) = NULL;
  rgl("glBindFramebuffer", (void **)&real);
  /* DIAG: ao SAIR do FBO da cena (fbo!=0 -> 0), lê o conteúdo do FBO p/ ver a
   * cena SEM o composite. Dump 1x após gameplay (g_draws_fbo já alto). */
  if (getenv("COI_FBO_DUMP") && g_cur_fbo != 0 && fb == 0 &&
      g_draws_fbo > 2000) {
    static int done = 0;
    if (!done) {
      done = 1;
      static void (*rp)(int,int,int,int,unsigned,unsigned,void*) = NULL;
      rgl("glReadPixels", (void **)&rp);
      int W = 1280, H = 720;
      unsigned char *buf = malloc((size_t)W * H * 4);
      if (rp && buf) {
        rp(0, 0, W, H, 0x1908, 0x1401, buf); /* RGBA, UBYTE */
        FILE *f = fopen("fbo_scene.raw", "wb");
        if (f) { fwrite(buf, 1, (size_t)W * H * 4, f); fclose(f); }
        fprintf(stderr, "[FBO_DUMP] FBO scene saved (fbo_scene.raw) draws_fbo=%lu\n", g_draws_fbo);
      }
      free(buf);
    }
  }
  g_cur_fbo = fb;
  if (real) real(tgt, fb);
}
/* ---- COI: rastro do ESTADO DE ATRIBUTOS por draw (diag Mickey preto) ----
 * Tabela idx->config; com gate /dev/shm/coi_uniflog ligado, loga UMA VEZ por
 * programa o layout completo no primeiro draw. */
static void coi_attr_dump_once(unsigned prog, int cnt) {
  static unsigned seen[128];
  static int sn = 0;
  if (!getenv("COI_UNIF_LOG") || access("/dev/shm/coi_uniflog", F_OK) != 0)
    return;
  for (int i = 0; i < sn; i++)
    if (seen[i] == prog) return;
  if (sn < 128) seen[sn++] = prog;
  fprintf(stderr, "[DRAWATTR] prog=%u cnt=%d tex0=%u tex1=%u tex2=%u "
          "depth=%d/0x%x dmask=%d blend=%d(0x%x,0x%x) cull=%d sten=%d cmask=0x%x\n",
          prog, cnt, g_bound_tex[0], g_bound_tex[1], g_bound_tex[2],
          g_st_depth, g_st_depthfunc, g_st_depthmask, g_st_blend, g_st_bsrc,
          g_st_bdst, g_st_cull, g_st_stencil, g_st_cmask);
  { /* leitura DIRETA dos uniforms de luz do programa no momento do draw */
    static int (*gul)(unsigned, const char *) = NULL;
    static void (*guf)(unsigned, int, float *) = NULL;
    rgl("glGetUniformLocation", (void **)&gul);
    rgl("glGetUniformfv", (void **)&guf);
    if (gul && guf) {
      const char *nm[] = {"g_CharacterLightColour", "g_Ambient", "_Color",
                          "g_RimLightColour", "lightsCol_Dir[0]",
                          "lightsPos_Dir[0]", "lightsAtt_Dir[0]",
                          "lightsCol_Omni[0]", "lightsPos_Omni[0]",
                          "lightsAtt_Omni[0]", "u_bone_matrices[0]"};
      for (unsigned q = 0; q < sizeof(nm) / sizeof(nm[0]); q++) {
        int L = gul(prog, nm[q]);
        if (L >= 0) {
          float v[4] = {0, 0, 0, 0};
          guf(prog, L, v);
          fprintf(stderr, "[DRAWUNIF] prog=%u %s(loc%d)=%.3f,%.3f,%.3f,%.3f\n",
                  prog, nm[q], L, v[0], v[1], v[2], v[3]);
        }
      }
    }
  }
  for (int i = 0; i < 16; i++)
    if (g_coi_attr[i].en || g_coi_attr[i].stride)
      fprintf(stderr,
              "[DRAWATTR]   idx=%d en=%d size=%d type=0x%x norm=%d stride=%d off=%ld buf=%u\n",
              i, g_coi_attr[i].en, g_coi_attr[i].size, g_coi_attr[i].type,
              g_coi_attr[i].norm, g_coi_attr[i].stride, g_coi_attr[i].off,
              g_coi_attr[i].buf);
}
static void my_glEnableVertexAttribArray(unsigned i) {
  static void (*real)(unsigned) = NULL;
  rgl("glEnableVertexAttribArray", (void **)&real);
  if (i < 16) g_coi_attr[i].en = 1;
  if (real) real(i);
}
static void my_glDisableVertexAttribArray(unsigned i) {
  static void (*real)(unsigned) = NULL;
  rgl("glDisableVertexAttribArray", (void **)&real);
  if (i < 16) g_coi_attr[i].en = 0;
  if (real) real(i);
}

static void my_glDrawElements(unsigned mode, int cnt, unsigned typ, const void *idx) {
  static void (*real)(unsigned, int, unsigned, const void *) = NULL;
  rgl("glDrawElements", (void **)&real);
  coi_attr_dump_once(g_cur_prog, cnt);
  if (g_cur_fbo) g_draws_fbo++; else g_draws_screen++;
  static unsigned long t = 0;
  static int stats = -1;
  if (stats < 0) stats = getenv("AS_GL_STATS") ? 1 : 0;
  if (stats && (++t % 3000) == 0)
    fprintf(stderr, "[DRAWSTATS] fbo=%lu screen=%lu (cur_fbo=%u)\n",
            g_draws_fbo, g_draws_screen, g_cur_fbo);
  static int drawlog = -1;
  if (drawlog < 0) drawlog = getenv("COI_DRAW_LOG") ? 1 : 0;
  if (drawlog) {
    static int dn = 0;
    /* draws GRANDES (chão/terreno) dentro do FBO: loga programa + texturas */
    if (g_cur_fbo && cnt > 3000 && dn < 30) {
      fprintf(stderr, "[BIGDRAW] prog=%u cnt=%d unit=%u tex[0]=%u tex[1]=%u\n",
              g_cur_prog, cnt, g_active_unit, g_bound_tex[0], g_bound_tex[1]);
      dn++;
    }
  }
  if (real && typ == 0x1405 /* GL_UNSIGNED_INT */ && cnt > 0 &&
      !as_gl_supports_uint_indices() &&
      g_cur_element_buf > 0 && g_cur_element_buf < AS_GL_TRACKED_BUFFERS) {
    AsElementBufferCopy *copy = &g_as_element_buffers[g_cur_element_buf];
    size_t offset = (size_t)(uintptr_t)idx;
    size_t count = (size_t)cnt;
    if (copy->mirror_valid && copy->mirror_id && (offset & 3u) == 0 &&
        offset <= copy->size &&
        count <= (copy->size - offset) / sizeof(uint32_t)) {
      static void (*real_bind)(unsigned, unsigned) = NULL;
      rgl("glBindBuffer", (void **)&real_bind);
      if (real_bind) {
        /* The mirror is uploaded once when BufferData changes.  Offsets in the
         * uint16 buffer are exactly half their uint32 byte offsets. */
        real_bind(0x8893 /* GL_ELEMENT_ARRAY_BUFFER */, copy->mirror_id);
        real(mode, cnt, 0x1403 /* GL_UNSIGNED_SHORT */,
             (const void *)(uintptr_t)(offset / 2));
        real_bind(0x8893, g_cur_element_buf);
        static int mirror_logs = 0;
        if (mirror_logs++ < 8)
          fprintf(stderr,
                  "[INDEX16] draw count=%d ebo=%u mirror=%u offset=%zu\n",
                  cnt, g_cur_element_buf, copy->mirror_id, offset);
        return;
      }
    }
    if ((offset & 3u) == 0 && offset <= copy->size &&
        count <= (copy->size - offset) / sizeof(uint32_t)) {
      if (g_as_u16_capacity < count) {
        unsigned short *grown = realloc(g_as_u16_indices,
                                        count * sizeof(*g_as_u16_indices));
        if (grown) {
          g_as_u16_indices = grown;
          g_as_u16_capacity = count;
        }
      }
      if (g_as_u16_capacity >= count) {
        const uint32_t *src = (const uint32_t *)(copy->data + offset);
        int fits = 1;
        for (size_t i = 0; i < count; i++) {
          if (src[i] > 0xffffu) { fits = 0; break; }
          g_as_u16_indices[i] = (unsigned short)src[i];
        }
        if (fits) {
          static void (*real_bind)(unsigned, unsigned) = NULL;
          rgl("glBindBuffer", (void **)&real_bind);
          if (real_bind) {
            /* GLES2 permits client-side indices when no element buffer is
             * bound.  Restore the engine's binding immediately afterwards;
             * vertex-buffer and attribute state are not changed. */
            real_bind(0x8893 /* GL_ELEMENT_ARRAY_BUFFER */, 0);
            real(mode, cnt, 0x1403 /* GL_UNSIGNED_SHORT */, g_as_u16_indices);
            real_bind(0x8893, g_cur_element_buf);
            static int converted_logs = 0;
            if (converted_logs++ < 8)
              fprintf(stderr,
                      "[INDEX16] draw count=%d ebo=%u offset=%zu converted uint32->uint16\n",
                      cnt, g_cur_element_buf, offset);
            return;
          }
        }
      }
    }
    static int rejected_logs = 0;
    if (rejected_logs++ < 8)
      fprintf(stderr,
              "[INDEX16] cannot convert draw count=%d ebo=%u offset=%zu bytes=%zu\n",
              cnt, g_cur_element_buf, offset, copy->size);
  }
  if (real) real(mode, cnt, typ, idx);
}
static void my_glDrawArrays(unsigned mode, int first, int cnt) {
  static void (*real)(unsigned, int, int) = NULL;
  rgl("glDrawArrays", (void **)&real);
  if (g_cur_fbo) g_draws_fbo++; else g_draws_screen++;
  if (real) real(mode, first, cnt);
}
void coi_draw_stats(void) {
  fprintf(stderr, "[DRAWSTATS] fbo=%lu screen=%lu (cur_fbo=%u)\n",
          g_draws_fbo, g_draws_screen, g_cur_fbo);
}

static void my_glFramebufferTexture2D(unsigned tgt, unsigned att, unsigned ttgt,
                                      unsigned tex, int lvl) {
  static void (*real)(unsigned, unsigned, unsigned, unsigned, int) = NULL;
  rgl("glFramebufferTexture2D", (void **)&real);
  if (real) real(tgt, att, ttgt, tex, lvl);
  static int n = 0;
  if (n < 30) { fprintf(stderr, "[FBO] FramebufferTexture2D att=0x%x tex=%u\n", att, tex); n++; }
}
/* ===== T2: RESOLUÇÃO INTERNA ===== escala o FBO de cena (cor+depth) por g_iscale
 * e o viewport quando renderiza nele; o composite p/ tela (fbo 0) faz o upscale.
 * 89% dos draws são no FBO de cena -> ataca o fill-rate do Utgard (ganho de fps).
 * COI_ISCALE (0.5-1.0; default 1.0 = OFF). Mira só alvos ~do tamanho da tela. */
static float g_iscale = -1.0f;
static int iscale_on(void) {
  if (g_iscale < 0) {
    const char *e = getenv("COI_ISCALE");
    if (e) g_iscale = (float)atof(e);
    else {
      /* AUTO: só vale em janela GRANDE (>=960 de largura, ex 720p). Em painel
       * pequeno (R36S 640x480) a cena interna já é ~0.6× — reduzir de novo =
       * borrado visível sem ganho real (validado 2026-07-02: título 60fps sem). */
      const char *a = getenv("COI_ISCALE_AUTO");
      g_iscale = (a && COI_W >= 960) ? (float)atof(a) : 1.0f;
    }
    if (g_iscale < 0.4f || g_iscale > 1.0f) g_iscale = 1.0f;
    if (g_iscale < 0.999f) fprintf(stderr, "[ISCALE] internal resolution = %.2f\n", g_iscale);
  }
  return g_iscale < 0.999f;
}
/* alvo do FBO de CENA apenas: a engine renderiza a cena em ~0.6× da janela (768x432
 * em 720p; 384x288 em 640x480) e compõe com upscale NORMALIZADO (UV 0-1) -> escalar é
 * seguro. FBOs de UI/dialogo em TAMANHO CHEIO da janela (missões, seleção de emoji)
 * são amostrados por sub-rect em PIXELS -> escalar dá ZOOM estourando a tela (bug
 * visto no R36S 2026-07-02). Faixa: 40%-85% da janela = só a cena. */
static int screenish(int w, int h) {
  return w >= COI_W * 2 / 5 && w <= COI_W * 17 / 20 &&
         h >= COI_H * 2 / 5 && h <= COI_H * 17 / 20;
}
static int iscaled(int v) { int r = (int)(v * g_iscale + 0.5f); r &= ~1; return r < 2 ? 2 : r; }

static void my_glViewport(int x, int y, int w, int h) {
  static void (*real)(int, int, int, int) = NULL;
  rgl("glViewport", (void **)&real);
  if (iscale_on() && g_cur_fbo != 0 && screenish(w, h)) {
    w = iscaled(w); h = iscaled(h);  /* renderiza no FBO de cena reduzido */
  }
  if (real) real(x, y, w, h);
}

static void my_glRenderbufferStorage(unsigned tgt, unsigned ifmt, int w, int h) {
  static void (*real)(unsigned, unsigned, int, int) = NULL;
  rgl("glRenderbufferStorage", (void **)&real);
  if (iscale_on() && screenish(w, h)) { w = iscaled(w); h = iscaled(h); }  /* depth de cena */
  if (real) real(tgt, ifmt, w, h);
  static int n = 0;
  if (n < 30) { fprintf(stderr, "[FBO] RenderbufferStorage ifmt=0x%x %dx%d\n", ifmt, w, h); n++; }
}
static unsigned my_glCheckFramebufferStatus(unsigned tgt) {
  static unsigned (*real)(unsigned) = NULL;
  rgl("glCheckFramebufferStatus", (void **)&real);
  unsigned s = real ? real(tgt) : 0x8CD5;
  static int n = 0;
  if (s != 0x8CD5 /*COMPLETE*/ || n < 30) {
    fprintf(stderr, "[FBO] CheckFramebufferStatus -> 0x%x %s\n", s,
            s == 0x8CD5 ? "COMPLETE" : "INCOMPLETO!!!");
    n++;
  }
  return s;
}

/* trace de criação de buffer + thread (worker thread sem contexto GL?) */
static void my_glBufferData(unsigned tgt, long size, const void *data, unsigned usage) {
  static void (*real)(unsigned, long, const void *, unsigned) = NULL;
  rgl("glBufferData", (void **)&real);
  static unsigned (*gerr)(void) = NULL; rgl("glGetError", (void **)&gerr);
  if (gerr) while (gerr()) {}
  if (real) real(tgt, size, data, usage);
  unsigned e = gerr ? gerr() : 0;
  if (tgt == 0x8893 /* GL_ELEMENT_ARRAY_BUFFER */ &&
      g_cur_element_buf < AS_GL_TRACKED_BUFFERS) {
    AsElementBufferCopy *copy = &g_as_element_buffers[g_cur_element_buf];
    copy->size = 0;
    copy->mirror_valid = 0;
    if (!e && data && size > 0) {
      size_t needed = (size_t)size;
      if (copy->capacity < needed) {
        unsigned char *grown = realloc(copy->data, needed);
        if (grown) {
          copy->data = grown;
          copy->capacity = needed;
        }
      }
      if (copy->capacity >= needed) {
        memcpy(copy->data, data, needed);
        copy->size = needed;
      }

      /* Cache a GPU-resident uint16 mirror.  The old path converted and sent
       * client-side indices on every draw; gameplay has hundreds of such draws
       * per frame.  Rebuild the mirror only when the engine replaces its EBO. */
      if (!as_gl_supports_uint_indices() && (needed & 3u) == 0) {
        size_t count = needed / sizeof(uint32_t);
        if (g_as_u16_capacity < count) {
          unsigned short *grown = realloc(
              g_as_u16_indices, count * sizeof(*g_as_u16_indices));
          if (grown) {
            g_as_u16_indices = grown;
            g_as_u16_capacity = count;
          }
        }
        int fits = g_as_u16_capacity >= count;
        const uint32_t *src = (const uint32_t *)data;
        for (size_t i = 0; fits && i < count; i++) {
          if (src[i] > 0xffffu) {
            fits = 0;
            break;
          }
          g_as_u16_indices[i] = (unsigned short)src[i];
        }
        if (fits) {
          static void (*real_gen)(int, unsigned *) = NULL;
          static void (*real_bind)(unsigned, unsigned) = NULL;
          rgl("glGenBuffers", (void **)&real_gen);
          rgl("glBindBuffer", (void **)&real_bind);
          if (!copy->mirror_id && real_gen) real_gen(1, &copy->mirror_id);
          if (copy->mirror_id && real_bind && real) {
            if (gerr) while (gerr()) {}
            real_bind(0x8893, copy->mirror_id);
            real(0x8893, (long)(count * sizeof(*g_as_u16_indices)),
                 g_as_u16_indices, usage);
            unsigned mirror_error = gerr ? gerr() : 0;
            real_bind(0x8893, g_cur_element_buf);
            copy->mirror_valid = mirror_error == 0;
            if (mirror_error)
              fprintf(stderr,
                      "[INDEX16] mirror upload failed ebo=%u mirror=%u err=0x%x\n",
                      g_cur_element_buf, copy->mirror_id, mirror_error);
          }
        }
      }
    }
  }
  /* DETECTA o stride real do buffer de vértice pela estrutura: o stride certo
   * deixa as UVs (offset 16, 2 floats) consistentemente em ~[0,1] p/ vários
   * vértices. Escolhe o MENOR stride com >85% de UVs válidas. */
  /* registra o TAMANHO do buffer (p/ deduzir stride por divisibilidade no draw) */
  if (tgt == 0x8892 && g_cur_array_buf < 4096) g_buf_size[g_cur_array_buf] = size;
  /* COI diag Mickey: dump hex dos 2 primeiros vertices dos buffers stride-44
   * (mesh skinned; cor ubyte4 deveria estar no off 40..43) */
  if (getenv("COI_UNIF_LOG") && tgt == 0x8892 && data && size > 20000 &&
      (size % 44) == 0) {
    static int vn = 0;
    if (vn < 6) {
      vn++;
      const unsigned char *b = (const unsigned char *)data;
      char hx[3 * 48 + 8];
      for (int v = 0; v < 2; v++) {
        int off = v * 44, hp = 0;
        for (int k = 0; k < 44; k++) hp += sprintf(hx + hp, "%02x", b[off + k]);
        fprintf(stderr, "[VB44] buf=%u size=%ld v%d=%s\n", g_cur_array_buf, size, v, hx);
      }
    }
  }
  /* TESTE: tinge a cor-de-vértice (rgba8 @12) dos buffers GRANDES (terreno,
   * stride 40) de verde — se o chão ficar verde, vary_color branco é o muro. */
  if (tgt == 0x8892 && data && getenv("COI_TINT_GREEN") && size > 2000 &&
      (size % 40) == 0) {
    unsigned char *b = (unsigned char *)data; /* nota: data é const; copiamos */
    static unsigned char *cp = NULL; static long cpsz = 0;
    if (cpsz < size) { free(cp); cp = malloc(size); cpsz = size; }
    if (cp) {
      memcpy(cp, b, size);
      for (long v = 0; v + 40 <= size; v += 40) { cp[v+12]=40; cp[v+13]=180; cp[v+14]=60; }
      if (real) real(tgt, size, cp, usage); /* re-upload tingido (sobrescreve) */
      static int tn = 0; if (tn < 3) { fprintf(stderr, "[TINT] buffer %ld verde\n", size); tn++; }
    }
  }
  static int n = 0;
  if (getenv("COI_BUF_LOG") && (n < 60 || e)) {
    fprintf(stderr, "[BUFDATA] tid=%d tgt=0x%x size=%ld data=%s usage=0x%x -> err=0x%x\n",
            (int)syscall(178), tgt, size, data ? "ptr" : "NULL", usage, e); n++;
  }
  /* dump dos vértices do buffer GIGANTE (chão): pos vec3@0 + cor rgba8@12 +
   * uv vec2@16, stride 24. Vê se UVs/cor estão válidas. */
  if (getenv("COI_VERT_DUMP") && data && size > 200000 && tgt == 0x8892) {
    static int vd = 0;
    if (vd < 2) {
      const unsigned char *b = (const unsigned char *)data;
      fprintf(stderr, "[VERTDUMP] buffer size=%ld (~%ld verts stride24)\n", size, size/24);
      for (int v = 0; v < 6; v++) {
        const float *pos = (const float *)(b + v*24);
        const unsigned char *col = b + v*24 + 12;
        const float *uv = (const float *)(b + v*24 + 16);
        fprintf(stderr, "  v%d: pos(%.1f,%.1f,%.1f) col(%d,%d,%d,%d) uv(%.3f,%.3f)\n",
                v, pos[0],pos[1],pos[2], col[0],col[1],col[2],col[3], uv[0],uv[1]);
      }
      vd++;
    }
  }
}

static void my_glDeleteBuffers(int n, const unsigned *ids) {
  static void (*real)(int, const unsigned *) = NULL;
  rgl("glDeleteBuffers", (void **)&real);
  if (ids) {
    for (int i = 0; i < n; i++) {
      unsigned id = ids[i];
      if (id >= AS_GL_TRACKED_BUFFERS) continue;
      AsElementBufferCopy *copy = &g_as_element_buffers[id];
      if (copy->mirror_id && real) real(1, &copy->mirror_id);
      free(copy->data);
      memset(copy, 0, sizeof(*copy));
    }
  }
  if (real) real(n, ids);
}

/* DIAG luz/sol: loga blends usados + permite forçar/desligar passes aditivos
 * (luz do player/sol estourada lavando o terreno de branco). */
static void my_glBlendFunc(unsigned sf, unsigned df) {
  g_st_bsrc = sf; g_st_bdst = df;
  static void (*real)(unsigned, unsigned) = NULL; rgl("glBlendFunc", (void **)&real);
  static int seen[64]; static int sn = 0;
  if (getenv("COI_BLEND_LOG")) {
    int key = (sf<<16)|df, known=0;
    for (int i=0;i<sn;i++) if(seen[i]==key){known=1;break;}
    if(!known && sn<64){seen[sn++]=key; fprintf(stderr,"[BLEND] src=0x%x dst=0x%x\n",sf,df);}
  }
  /* =1: troca aditivo (ONE,ONE / SRC_ALPHA,ONE) por alpha normal (teste luz) */
  if (getenv("COI_NO_ADD")) {
    if ((sf==1 && df==1) || (sf==0x302 && df==1)) { sf=0x302; df=0x303; } /* SRC_ALPHA,1-SRC_ALPHA */
  }
  if (real) real(sf, df);
}
static void my_glBlendFuncSeparate(unsigned sf, unsigned df, unsigned sa, unsigned da) {
  g_st_bsrc = sf; g_st_bdst = df;
  static void (*real)(unsigned, unsigned, unsigned, unsigned) = NULL;
  rgl("glBlendFuncSeparate", (void **)&real);
  static int seen[64]; static int sn = 0;
  if (getenv("COI_BLEND_LOG")) {
    int key = (sf<<16)|df, known=0;
    for (int i=0;i<sn;i++) if(seen[i]==key){known=1;break;}
    if(!known && sn<64){seen[sn++]=key; fprintf(stderr,"[BLENDSEP] src=0x%x dst=0x%x srcA=0x%x dstA=0x%x\n",sf,df,sa,da);}
  }
  if (getenv("COI_NO_ADD")) {
    if ((sf==1 && df==1) || (sf==0x302 && df==1)) { sf=0x302; df=0x303; }
  }
  if (real) real(sf, df, sa, da);
}
/* DIAG depth: força glDepthFunc=ALWAYS (depth-texture FBO quebrado no Utgard?
 * → mundo culado). COI_DEPTH_ALWAYS=1. =2 desliga depth test inteiro. */
static void my_glDepthFunc(unsigned f) {
  static void (*real)(unsigned) = NULL; rgl("glDepthFunc", (void **)&real);
  g_st_depthfunc = f;
  if (getenv("COI_DEPTH_ALWAYS")) f = 0x0207; /* GL_ALWAYS */
  /* Castle needed this only for a proven character-lighting quirk.  Action
   * Squad keeps the native depth function unless explicitly diagnosing it. */
  if (f == 0x0202 && getenv("AS_DEPTH_EQUAL_LEQUAL")) {
    static int dn = 0;
    if (dn < 4) { fprintf(stderr, "[DEPTHEQ] GL_EQUAL -> GL_LEQUAL\n"); dn++; }
    f = 0x0203;
  }
  if (real) real(f);
}
static void coi_track_cap(unsigned cap, int v) {
  if (cap == 0x0BE2) g_st_blend = v;        /* BLEND */
  if (cap == 0x0B71) g_st_depth = v;        /* DEPTH_TEST */
  if (cap == 0x0B44) g_st_cull = v;         /* CULL_FACE */
  if (cap == 0x0B90) g_st_stencil = v;      /* STENCIL_TEST */
}
static void my_glDisable(unsigned cap) {
  static void (*real)(unsigned) = NULL; rgl("glDisable", (void **)&real);
  coi_track_cap(cap, 0);
  if (real) real(cap);
}
static void my_glDepthMask(unsigned char m) {
  static void (*real)(unsigned char) = NULL; rgl("glDepthMask", (void **)&real);
  g_st_depthmask = m ? 1 : 0;
  if (real) real(m);
}
static void my_glColorMask(unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
  static void (*real)(unsigned char, unsigned char, unsigned char, unsigned char) = NULL;
  rgl("glColorMask", (void **)&real);
  g_st_cmask = (r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0);
  if (real) real(r, g, b, a);
}
static void my_glEnable(unsigned cap) {
  static void (*real)(unsigned) = NULL; rgl("glEnable", (void **)&real);
  coi_track_cap(cap, 1);
  const char *d = getenv("COI_DEPTH_ALWAYS");
  if (d && atoi(d) == 2 && cap == 0x0B71 /*DEPTH_TEST*/) return; /* não habilita */
  if (real) real(cap);
}

/* DIAG: força clear color (magenta) p/ distinguir branco-geometria de fundo */
static void my_glClearColor(float r, float g, float b, float a) {
  static void (*real)(float, float, float, float) = NULL;
  rgl("glClearColor", (void **)&real);
  /* =1: tudo magenta. =2: só TELA (fbo 0) magenta. =3: só FBO magenta. */
  const char *t = getenv("COI_CLEAR_TEST");
  if (t) {
    int m = atoi(t);
    int hit = (m == 1) || (m == 2 && g_cur_fbo == 0) || (m == 3 && g_cur_fbo != 0);
    if (hit) { r = 1.0f; g = 0.0f; b = 1.0f; a = 1.0f; }
  }
  if (real) real(r, g, b, a);
}

/* glTexParameteri: Mali-450 GLES2 NÃO completa textura NPOT com GL_REPEAT nem
 * com min-filter mipmap → amostra branco. Forçamos CLAMP_TO_EDGE + filtro
 * não-mipmap (NPOT-safe). COI_NPOT_OFF desliga. */
static int g_npot_fix = -1;
static void my_glTexParameteri(unsigned tgt, unsigned pname, int param) {
  static void (*real)(unsigned, unsigned, int) = NULL;
  rgl("glTexParameteri", (void **)&real);
  /* 🔑 RAIZ DO MICKEY/PORTA PRETOS: o npot_fix (herdado do scaffold) forçava
   * WRAP=CLAMP_TO_EDGE em toda textura; materiais do COI com UV espelhado/
   * repetido (personagem, portas mirror_plane, plumas) grudavam na borda do
   * atlas -> albedo errado/escuro. O COI usa POT+mips completos do OBB e o
   * Mali-450 tem OES_texture_npot real -> default OFF (ligar: COI_NPOT_FIX=1). */
  if (g_npot_fix < 0) g_npot_fix = getenv("COI_NPOT_FIX") ? 1 : 0;
  static int n = 0;
  if (getenv("COI_TEXPARAM_LOG") && n < 40) {
    fprintf(stderr, "[TEXPARAM] pname=0x%x param=0x%x\n", pname, param); n++;
  }
  if (g_npot_fix) {
    if (pname == 0x2802 /*WRAP_S*/ || pname == 0x2803 /*WRAP_T*/)
      param = 0x812F; /* CLAMP_TO_EDGE */
    else if (pname == 0x2801 /*MIN_FILTER*/) {
      if (param == 0x2700 || param == 0x2701 || param == 0x2702 || param == 0x2703)
        param = 0x2601; /* mipmap → LINEAR */
    }
  }
  if (real) real(tgt, pname, param);
}
/* ========== CACHE ETC1 OFFLINE (sidetable do texbake) ==========
 * A engine carrega o .jpg/.png normal (imagem completa, sem crash/pink). Aqui, no
 * upload, em vez de ENCODAR ETC1 em runtime (stutter), fazemos LOOKUP por NOME da
 * ETC1 já pré-bakeada -> ZERO encode. Caminho do SOR4: controlar no nosso hook. */
extern const char *bk_last_bmp_name(void);
typedef struct { const char *name; int w, h; const unsigned char *blob; int size; } EtcEnt;
static EtcEnt *g_etc = NULL; static long g_netc = -1;   /* -1 = ainda não tentou carregar */
static unsigned char *g_etcfile = NULL;
static int g_etc_zlib = 0;   /* 1 = blobs do cache estão comprimidos (zlib) */
static int (*g_zuncompress)(unsigned char *, unsigned long *, const unsigned char *, unsigned long) = NULL;
static int etc_name_cmp(const char *a, const char *b) {  /* bytewise (igual texbake) */
  const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
  while (*x && *x == *y) { x++; y++; }
  return (int)*x - (int)*y;
}
static void etc1cache_load(void) {
  g_netc = 0;
  const char *path = getenv("COI_ETC1CACHE");
  if (!path) return;
  /* 🔑 mmap (NÃO malloc+fread): o cache tem centenas de MB; carregá-lo inteiro na RAM
   * estouraria 1GB. mmap deixa as páginas no disco e só residem as ACESSADAS (cada
   * textura é lida 1× no upload e a página pode ser evictada). */
  int fd = open(path, O_RDONLY);
  if (fd < 0) { fprintf(stderr, "[ETC1CACHE] could not open %s\n", path); return; }
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size < 16) { close(fd); return; }
  long n = (long)st.st_size;
  g_etcfile = (unsigned char *)mmap(NULL, n, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (g_etcfile == MAP_FAILED) { g_etcfile = NULL; fprintf(stderr, "[ETC1CACHE] mmap failed\n"); return; }
  if (!memcmp(g_etcfile, "ETC1CAZ1", 8)) {        /* cache COMPRIMIDO (zlib) */
    g_etc_zlib = 1;
    void *z = dlopen("libz.so.1", RTLD_NOW); if (!z) z = dlopen("libz.so", RTLD_NOW);
    if (z) g_zuncompress = dlsym(z, "uncompress");
    if (!g_zuncompress) { fprintf(stderr, "[ETC1CACHE] zlib cache but no libz -> disabled\n"); return; }
  } else if (memcmp(g_etcfile, "ETC1CACH", 8)) { fprintf(stderr, "[ETC1CACHE] magic ruim\n"); return; }
  uint32_t count = *(uint32_t *)(g_etcfile + 8), data_off = *(uint32_t *)(g_etcfile + 12);
  g_etc = (EtcEnt *)malloc((size_t)count * sizeof(EtcEnt));
  unsigned char *p = g_etcfile + 16;
  for (uint32_t i = 0; i < count; i++) {
    g_etc[i].name = (const char *)p; int L = (int)strlen((const char *)p); p += L + 1;
    g_etc[i].w = *(uint16_t *)p; p += 2; g_etc[i].h = *(uint16_t *)p; p += 2;
    uint32_t bo = *(uint32_t *)p; p += 4; g_etc[i].size = (int)(*(uint32_t *)p); p += 4;
    g_etc[i].blob = g_etcfile + data_off + bo;
  }
  g_netc = count;
  fprintf(stderr, "[ETC1CACHE] %ld ETC1 textures loaded from %s\n", g_netc, path);
}
static const EtcEnt *etc1cache_find(const char *name) {
  if (g_netc <= 0 || !name) return NULL;
  long lo = 0, hi = g_netc - 1;
  while (lo <= hi) { long m = (lo + hi) / 2; int c = etc_name_cmp(g_etc[m].name, name);
    if (c == 0) return &g_etc[m]; if (c < 0) lo = m + 1; else hi = m - 1; }
  return NULL;
}

/* 🔎 VERIFICAÇÃO DE CONTEÚDO (mata o magenta): o cache é indexado por NOME, mas o
 * nome (bk_last_bmp_name) pode estar VELHO entre uploads (mips/atlas/FBO sem nome).
 * Quando uma textura ERRADA bate a MESMA dim de uma entrada do cache (512²/256²/128²
 * são comuns), a guarda de tamanho passa e subiríamos a ETC1 ERRADA -> chão magenta.
 * Solução: antes de aceitar, DECODIFICA uma AMOSTRA de blocos ETC1 do cache e compara
 * com o RGBA REAL que a engine vai subir. Se o conteúdo bate (dentro da perda do ETC1)
 * -> é a textura certa, sobe. Se não bate -> colisão de nome -> recusa (cai pro RGBA8
 * correto). Decode por-bloco (4x4) em ~24 posições espalhadas = barato (load-time).
 * ETC1 cru rotula como ETC2-RGB (0x9274) e o decoder ETC2 decodifica idêntico (nossos
 * blocos só usam modos individual/differential, subconjunto válido do ETC2). */
static int etc1cache_content_ok(const EtcEnt *e, const unsigned char *blob, int blobsz,
                                const void *data, int ch) {
  extern unsigned char *etc2_decode_rgba(unsigned, int, int, const void *, int);
  int bw = e->w / 4, bh = e->h / 4;
  if (bw <= 0 || bh <= 0) return 0;
  (void)blobsz;
  const unsigned char *src = (const unsigned char *)data;
  /* amostra ~24 blocos espalhados em grade */
  int gx = bw < 5 ? bw : 5, gy = bh < 5 ? bh : 5;
  long sum = 0, cnt = 0; int sampled = 0;
  for (int iy = 0; iy < gy; iy++) {
    for (int ix = 0; ix < gx; ix++) {
      int bx = (int)((long)ix * (bw - 1) / (gx > 1 ? gx - 1 : 1));
      int by = (int)((long)iy * (bh - 1) / (gy > 1 ? gy - 1 : 1));
      const unsigned char *blk = blob + ((long)by * bw + bx) * 8;
      unsigned char *dec = etc2_decode_rgba(0x9274, 4, 4, blk, 8);
      if (!dec) continue;
      sampled++;
      for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) {
        long px = ((long)(by * 4 + j) * e->w + (bx * 4 + i));
        const unsigned char *s = src + px * ch;
        const unsigned char *d = dec + (j * 4 + i) * 4;
        int dr = d[0] - s[0], dg = d[1] - s[1], db = d[2] - s[2];
        sum += (dr < 0 ? -dr : dr) + (dg < 0 ? -dg : dg) + (db < 0 ? -db : db);
        cnt += 3;
      }
      free(dec);
    }
  }
  if (!sampled || !cnt) return -1;
  return (int)(sum / cnt);   /* erro médio absoluto por canal (caller decide o limite) */
}

/* T3: tenta subir a textura como ETC1 (0x8D64) em vez de RGBA8. Só texturas
 * OPACAS, mip 0, dim múltipla de 4. ~8× menos VRAM no Utgard (amostra ETC1 nativo).
 * 1º: LOOKUP no cache offline (sem encode) + VERIFICAÇÃO DE CONTEÚDO. SEM fallback de
 * encode em runtime no caminho de produção (nada convertido dentro do jogo).
 * Retorna 1 se subiu ETC1; 0 = caller faz o upload RGBA normal. COI_NO_ETC1 desliga. */
static int try_upload_etc1(unsigned tgt, int lvl, int w, int h,
                           const void *data, unsigned fmt) {
  static int en = -1;
  if (en < 0) en = getenv("COI_NO_ETC1") ? 0 : 1;
  if (!en || lvl != 0 || !data) return 0;

  static int diag = -1;
  if (diag < 0) diag = getenv("COI_CACHEDIAG") ? 1 : 0;
  static int dn = 0;
  /* limite do MAD pra aceitar a ETC1 do cache (tunável: COI_VERIFY_MAX). */
  static int ETC1_VERIFY_MAX = -1;
  if (ETC1_VERIFY_MAX < 0) { const char *v = getenv("COI_VERIFY_MAX"); ETC1_VERIFY_MAX = v ? atoi(v) : 28; }

  /* 🌑 NÃO comprimir MAPAS DE ILUMINAÇÃO em ETC1: normals/specular/heights guardam
   * dado por-canal (não cor). ETC1 (lossy, correlaciona RGB) destrói esse dado ->
   * iluminação errada -> chão/personagem/árvores PRETOS. Sobem RGBA8 (correto). */
  {
    const char *nm = bk_last_bmp_name();
    if (nm && (strstr(nm, "normal") || strstr(nm, "specular") || strstr(nm, "-spec") ||
               strstr(nm, "height") || strstr(nm, "-gloss") || strstr(nm, "roughness"))) {
      if (diag && dn < 600 && w >= 32) { fprintf(stderr, "[DIAG] EXCLUDED-LIGHT '%s' %dx%d\n", nm, w, h); dn++; }
      return 0;
    }
  }

  /* 🔑 CACHE OFFLINE: se esta textura (por nome) tem ETC1 pré-bakeada, sobe direto. */
  if (g_netc < 0) etc1cache_load();
  if (g_netc > 0 && (fmt == 0x1908 || fmt == 0x1907)) {
    const char *nm = bk_last_bmp_name();
    const EtcEnt *e = etc1cache_find(nm);
    if (diag && dn < 600 && w >= 32) {
      if (!e) { fprintf(stderr, "[DIAG] MISS-NONAME '%s' %dx%d\n", nm, w, h); dn++; }
      else if (e->w != w || e->h != h) { fprintf(stderr, "[DIAG] MISS-DIM '%s' up=%dx%d cache=%dx%d\n", nm, w, h, e->w, e->h); dn++; }
      else { fprintf(stderr, "[DIAG] HIT '%s' %dx%d\n", nm, w, h); dn++; }
    }
    /* 🛡️ GUARDA DE TAMANHO: só substitui se as dims do cache BATEM com o upload atual.
     * g_last_bmp_name pode estar VELHO (FBO/mip/upload sem nome) -> sem a guarda
     * subiríamos a ETC1 errada -> textura branca/preta/lixo no gameplay. */
    if (e && e->w == w && e->h == h) {
      static void (*rc)(unsigned, int, unsigned, int, int, int, int, const void *) = NULL;
      rgl("glCompressedTexImage2D", (void **)&rc);
      static unsigned (*ge)(void) = NULL; rgl("glGetError", (void **)&ge);
      if (ge) while (ge()) {}
      int ok = 0;
      const unsigned char *blob = e->blob; int blobsz = e->size;
      unsigned char *infl = NULL;
      /* 🗜️ cache COMPRIMIDO: infla o blob (tam. ETC1 = (w/4)*(h/4)*8, derivado de w,h). */
      if (g_etc_zlib) {
        unsigned long unc = (unsigned long)(e->w / 4) * (e->h / 4) * 8;
        infl = (unsigned char *)malloc(unc);
        unsigned long got = unc;
        if (infl && g_zuncompress(infl, &got, e->blob, e->size) == 0 && got == unc) {
          blob = infl; blobsz = (int)unc;
        } else { free(infl); infl = NULL; blob = NULL; }
      }
      /* 🔎 só sobe a ETC1 do cache se o CONTEÚDO bate com o RGBA real (anti-magenta).
       * MAD = erro médio por canal entre a ETC1 do cache e o RGBA que a engine vai subir.
       * textura CERTA: MAD baixo (perda do ETC1 ~3-20); colisão de nome: MAD alto (~40+). */
      int mad = blob ? etc1cache_content_ok(e, blob, blobsz, data, (fmt == 0x1908) ? 4 : 3) : -1;
      int verified = (mad >= 0 && mad <= ETC1_VERIFY_MAX);
      if (verified && rc && blob) { rc(tgt, 0, 0x8D64, e->w, e->h, 0, blobsz, blob); ok = (ge ? ge() : 1) == 0; }
      free(infl);
      /* 📊 contadores cumulativos (sem cap) p/ medir a taxa real de uso do cache. */
      static long n_ok = 0, n_rej = 0; if (ok) n_ok++; else if (mad >= 0) n_rej++;
      if (diag && ((n_ok + n_rej) % 500) == 0 && (n_ok + n_rej) > 0)
        fprintf(stderr, "[CACHESTATS] ETC1-usado=%ld rejeitado=%ld (%.0f%% aceito)\n", n_ok, n_rej, 100.0 * n_ok / (n_ok + n_rej));
      if (diag && dn < 600 && w >= 32 && !verified) { fprintf(stderr, "[DIAG] VERIFY-FAIL '%s' %dx%d MAD=%d (limite=%d)\n", nm, w, h, mad, ETC1_VERIFY_MAX); dn++; }
      static int cl = 0;
      if (cl < 12) { fprintf(stderr, "[ETC1CACHE] '%s' %dx%d -> %s\n", bk_last_bmp_name(), e->w, e->h, ok ? "ETC1(cache)" : (verified ? "failed" : "rejected(content)")); cl++; }
      if (ok) return 1;     /* subiu do cache verificado, sem encode */
    }
  }
  /* 🚫 SEM ENCODE EM RUNTIME no caminho de produção (nada convertido dentro do jogo):
   * cache-miss / dim-mismatch / colisão -> a engine sobe RGBA8 (cor correta). O encode
   * em runtime (stutter na CPU fraca) só liga com COI_RT_ENCODE=1 (diagnóstico). */
  if (!getenv("COI_RT_ENCODE")) return 0;
  if (fmt != 0x1908 && fmt != 0x1907) return 0;          /* só RGBA/RGB */
  if (w < 32 || h < 32 || (w & 3) || (h & 3)) return 0;  /* conteúdo, múltiplo de 4 */
  int ch = (fmt == 0x1908) ? 4 : 3;
  const unsigned char *p = (const unsigned char *)data;
  if (ch == 4) {                                          /* ETC1 não tem alpha: só opacas */
    long n = (long)w * h;
    for (long i = 0; i < n; i++) if (p[i * 4 + 3] < 250) return 0;
  }
  size_t sz = (size_t)(w / 4) * (h / 4) * 8;
  unsigned char *buf = (unsigned char *)malloc(sz);
  if (!buf) return 0;
  extern void etc1_encode_image(const unsigned char *, int, int, int, unsigned char *);
  etc1_encode_image(p, w, h, ch, buf);
  static void (*rc)(unsigned, int, unsigned, int, int, int, int, const void *) = NULL;
  rgl("glCompressedTexImage2D", (void **)&rc);
  static unsigned (*ge)(void) = NULL; rgl("glGetError", (void **)&ge);
  if (ge) while (ge()) {}
  int ok = 0;
  if (rc) { rc(tgt, 0, 0x8D64, w, h, 0, (int)sz, buf); ok = (ge ? ge() : 1) == 0; }
  free(buf);
  static int log = 0;
  if (log < 10) { fprintf(stderr, "[ETC1] %dx%d ch=%d -> %s\n", w, h, ch,
                          ok ? "ETC1" : "falhou->RGBA"); log++; }
  return ok;
}

static int g_tex_log = -1, g_tex_fix = -1;
static void my_glTexImage2D(unsigned tgt, int lvl, int ifmt, int w, int h,
                            int border, unsigned fmt, unsigned typ,
                            const void *px) {
  static void (*real)(unsigned, int, int, int, int, int, unsigned, unsigned,
                      const void *) = NULL;
  rgl("glTexImage2D", (void **)&real);
  static unsigned (*gerr)(void) = NULL; rgl("glGetError", (void **)&gerr);
  if (g_tex_log < 0) g_tex_log = getenv("COI_TEX_LOG") ? 1 : 0;
  if (g_tex_fix < 0) g_tex_fix = getenv("COI_TEX_NOFIX") ? 0 : 1;
  /* DYS_PAGE: id bindado agora (p/ registrar/unpage); redefinicao SEM pixels (RT) sai do paging */
  unsigned coip_tid = g_bound_tex[g_active_unit < 8 ? g_active_unit : 0];
  if (coi_paging() && tgt == 0x0DE1 && lvl == 0 && !px) coip_unpage(coip_tid);
  int orig = ifmt;
  if (g_tex_fix) {
    /* normaliza internalformat sized (GLES3) → base (GLES2) */
    switch (ifmt) {
      case 0x8058: /*RGBA8*/ case 0x8C43: /*SRGB8_ALPHA8*/ case 0x881A: /*RGBA16F*/
      case 0x8814: /*RGBA32F*/ ifmt = 0x1908; break; /* GL_RGBA */
      case 0x8051: /*RGB8*/ case 0x8C41: /*SRGB8*/ case 0x881B: /*RGB16F*/
      case 0x8815: /*RGB32F*/ ifmt = 0x1907; break; /* GL_RGB */
      case 0x8229: /*R8*/ case 0x822E: /*R32F*/ ifmt = 0x1909; break; /* LUMINANCE */
      case 0x822B: /*RG8*/ ifmt = 0x190A; break; /* LUMINANCE_ALPHA */
      default: break;
    }
  }
  /* 🏎️ COI_TEXSCALE=F: reduz TODA textura RGBA grande por fator F
   * (ex 1.2 = ~83% das dimensões, bilinear). Menos banda de memória/cache de
   * textura na GPU — ideia do porter p/ os milhares de itens no mapa. */
  {
    static float texscale = -1.0f;
    if (texscale < 0.0f) {
      const char *e = getenv("COI_TEXSCALE");
      texscale = e ? (float)atof(e) : 0.0f;
      if (texscale != 0.0f && texscale < 1.05f) texscale = 0.0f;
      if (texscale > 0.0f) fprintf(stderr, "[TEXSCALE] fator=%.2f\n", texscale);
    }
    if (texscale > 0.0f && px && lvl == 0 && fmt == 0x1908 && typ == 0x1401 &&
        w >= 128 && h >= 128) {
      /* 🔑 dims do downscale IDÊNTICAS ao texbake (round p/ múltiplo de 4: (n+2)&~3),
       * senão o cache ETC1 (bakeado na mesma escala) dá MISS-DIM e não é usado. */
      int nw = ((int)((float)w / texscale) + 2) & ~3, nh = ((int)((float)h / texscale) + 2) & ~3;
      if (nw >= 16 && nh >= 16 && (nw < w || nh < h)) {
        static unsigned char *sb = NULL; static long scap = 0;
        long need = (long)nw * nh * 4;
        if (scap < need) { free(sb); sb = malloc(need); scap = need; }
        if (sb) {
          const unsigned char *src = (const unsigned char *)px;
          for (int y = 0; y < nh; y++) {
            float fy = ((float)y + 0.5f) * h / nh - 0.5f;
            int y0 = (int)fy; if (y0 < 0) y0 = 0;
            int y1 = y0 + 1 < h ? y0 + 1 : h - 1;
            float wy = fy - y0;
            for (int x = 0; x < nw; x++) {
              float fx = ((float)x + 0.5f) * w / nw - 0.5f;
              int x0 = (int)fx; if (x0 < 0) x0 = 0;
              int x1 = x0 + 1 < w ? x0 + 1 : w - 1;
              float wx = fx - x0;
              const unsigned char *p00 = src + ((long)y0 * w + x0) * 4;
              const unsigned char *p01 = src + ((long)y0 * w + x1) * 4;
              const unsigned char *p10 = src + ((long)y1 * w + x0) * 4;
              const unsigned char *p11 = src + ((long)y1 * w + x1) * 4;
              unsigned char *d = sb + ((long)y * nw + x) * 4;
              for (int c = 0; c < 4; c++) {
                float t = p00[c] * (1 - wx) * (1 - wy) + p01[c] * wx * (1 - wy) +
                          p10[c] * (1 - wx) * wy + p11[c] * wx * wy;
                d[c] = (unsigned char)(t + 0.5f);
              }
            }
          }
          if (gerr) while (gerr()) {}
          /* DYS_PAGE ativo: pula ETC1 lossy (a paginacao segura a RAM; qualidade nativa) */
          if (!coi_paging() && try_upload_etc1(tgt, lvl, nw, nh, sb, fmt)) return;  /* T3 */
          if (real) real(tgt, lvl, ifmt, nw, nh, border, fmt, typ, sb);
          if (tgt == 0x0DE1 && lvl == 0)
            coip_note_upload(coip_tid, ifmt, nw, nh, fmt, typ, sb, (unsigned)((long)nw * nh * 4));
          static int sn = 0;
          if (sn < 6) { fprintf(stderr, "[TEXSCALE] %dx%d -> %dx%d\n", w, h, nw, nh); sn++; }
          return;
        }
      }
    }
  }
  /* 🧠 TEORIA MEMÓRIA UTGARD (Bully): muitas/grandes texturas estouram a memória
   * de textura da GPU → uploads tardios (terreno) falham/somem. COI_TEX_HALF
   * reduz texturas grandes pela metade (box 2x2) p/ liberar memória. */
  if (getenv("COI_TEX_HALF") && px && lvl == 0 && fmt == 0x1908 &&
      typ == 0x1401 && w >= 256 && h >= 256 && (w & 1) == 0 && (h & 1) == 0) {
    int hw = w / 2, hh = h / 2;
    static unsigned char *half = NULL; static long hcap = 0;
    long need = (long)hw * hh * 4;
    if (hcap < need) { free(half); half = malloc(need); hcap = need; }
    if (half) {
      const unsigned char *src = (const unsigned char *)px;
      for (int y = 0; y < hh; y++)
        for (int x = 0; x < hw; x++) {
          long s0 = ((long)(y*2)*w + x*2)*4, s1 = s0+4, s2 = s0+(long)w*4, s3 = s2+4;
          unsigned char *d = half + ((long)y*hw + x)*4;
          for (int c = 0; c < 4; c++) d[c] = (src[s0+c]+src[s1+c]+src[s2+c]+src[s3+c])/4;
        }
      if (gerr) while (gerr()) {}
      if (real) real(tgt, lvl, ifmt, hw, hh, border, fmt, typ, half);
      static int hn = 0; if (hn < 4) { fprintf(stderr, "[TEXHALF] %dx%d -> %dx%d\n", w, h, hw, hh); hn++; }
      return;
    }
  }
  /* T2: textura de COR do FBO de cena (sem dados, ~768x432) -> reduzida */
  if (iscale_on() && !px && lvl == 0 && screenish(w, h)) {
    int nw = iscaled(w), nh = iscaled(h);
    if (real) real(tgt, lvl, ifmt, nw, nh, border, fmt, typ, NULL);
    static int vn = 0;
    if (vn < 6) { fprintf(stderr, "[ISCALE] FBO color %dx%d -> %dx%d\n", w, h, nw, nh); vn++; }
    return;
  }
  if (gerr) while (gerr()) {}
  /* DYS_PAGE ativo: pula ETC1 lossy (paginacao segura a RAM; qualidade nativa) */
  if (!coi_paging() && try_upload_etc1(tgt, lvl, w, h, px, fmt)) return;  /* T3 */
  if (real) real(tgt, lvl, ifmt, w, h, border, fmt, typ, px);
  if (px && tgt == 0x0DE1 && lvl == 0) {
    int _bpp = coip_bpp(fmt, typ);
    if (_bpp > 0) coip_note_upload(coip_tid, ifmt, w, h, fmt, typ, px, (unsigned)((long)w * h * _bpp));
  }
  unsigned e = gerr ? gerr() : 0;
  if (getenv("COI_TEXID"))
    fprintf(stderr, "[TEXID] U id=%u lvl=%d ifmt=0x%x fmt=0x%x typ=0x%x %dx%d err=0x%x\n",
            coip_tid, lvl, ifmt, fmt, typ, w, h, e);
  if (g_tex_log) {
    /* histograma de (fmt,typ) distintos — pega LUMINANCE/ALPHA escondidos */
    static unsigned seen_fmt[32]; static int nf = 0;
    unsigned key = (fmt << 8) | (typ & 0xff);
    int known = 0;
    for (int i = 0; i < nf; i++) if (seen_fmt[i] == key) { known = 1; break; }
    if (!known && nf < 32) {
      seen_fmt[nf++] = key;
      const char *fn = fmt==0x1908?"RGBA":fmt==0x1907?"RGB":fmt==0x1909?"LUMINANCE":
                       fmt==0x190A?"LUMINANCE_ALPHA":fmt==0x1906?"ALPHA":"?";
      fprintf(stderr, "[TEXFMT] fmt=0x%x(%s) typ=0x%x\n", fmt, fn, typ);
    }
    static int n = 0;
    unsigned tid = g_bound_tex[g_active_unit < 8 ? g_active_unit : 0];
    /* salva a imagem inteira de texturas grandes p/ inspeção (COI_TEX_SAVE) */
    if (px && lvl == 0 && getenv("COI_TEX_SAVE") && w >= 128 && h >= 128 && fmt == 0x1908) {
      static int sv = 0;
      if (sv < 80) {
        char nm[80]; snprintf(nm, sizeof(nm), "texdump_t%u_%dx%d.raw", tid, w, h);
        FILE *tf = fopen(nm, "wb");
        if (tf) { fwrite(px, 1, (size_t)w*h*4, tf); fclose(tf);
          fprintf(stderr, "[TEXSAVE] %s\n", nm); sv++; }
      }
    }
    if (px && lvl == 0 && w >= 64 && h >= 64 && fmt == 0x1908) {
      const unsigned char *b = (const unsigned char *)px;
      char grid[160]; int gi = 0; int colorful = 0;
      for (int k = 0; k < 6; k++) {
        long o = ((long)((k*7+3)%h * 0 + (k+1)*h/8) * w + (k+1)*w/8) * 4;
        int R=b[o],G=b[o+1],B=b[o+2];
        int mx = R>G?(R>B?R:B):(G>B?G:B), mn = R<G?(R<B?R:B):(G<B?G:B);
        if (mx - mn > 24) colorful++;
        gi += snprintf(grid+gi, sizeof(grid)-gi, "%02x%02x%02x ", R,G,B);
      }
      static int gn = 0;
      if (gn < 30) { fprintf(stderr, "[TEXRGB] tex=%u %dx%d %s rgb: %s\n",
                             tid, w, h, colorful>=2?"COLORIDA":"grayscale", grid); gn++; }
    }
    if (n < 80 || e) {
      char pix[80] = "(null)";
      if (px && lvl == 0 && w >= 4 && h >= 4) {
        const unsigned char *b = (const unsigned char *)px;
        long mid = ((long)h / 2 * w + w / 2) * 4;
        snprintf(pix, sizeof(pix), "p0=%02x%02x%02x%02x mid=%02x%02x%02x%02x",
                 b[0], b[1], b[2], b[3], b[mid], b[mid+1], b[mid+2], b[mid+3]);
      }
      fprintf(stderr, "[TEX2D] tid=%d tex=%u ifmt=0x%x %dx%d fmt=0x%x typ=0x%x -> err=0x%x %s\n",
              (int)syscall(178), tid, ifmt, w, h, fmt, typ, e, pix); n++;
    }
  }
}
static void my_glTexImage3D(unsigned tgt, int lvl, int ifmt, int w, int h,
                            int d, int border, unsigned fmt, unsigned typ,
                            const void *px) {
  static void (*real)(unsigned, int, int, int, int, int, int, unsigned,
                      unsigned, const void *) = NULL;
  rgl("glTexImage3D", (void **)&real);
  fprintf(stderr, "[TEX3D] glTexImage3D tgt=0x%x ifmt=0x%x %dx%dx%d fmt=0x%x\n",
          tgt, ifmt, w, h, d, fmt);
  if (real) real(tgt, lvl, ifmt, w, h, d, border, fmt, typ, px);
}
static void my_glTexStorage3D(unsigned tgt, int lvls, unsigned ifmt, int w,
                              int h, int d) {
  static void (*real)(unsigned, int, unsigned, int, int, int) = NULL;
  rgl("glTexStorage3D", (void **)&real);
  fprintf(stderr, "[TEX3D] glTexStorage3D tgt=0x%x ifmt=0x%x %dx%dx%d\n", tgt,
          ifmt, w, h, d);
  if (real) real(tgt, lvls, ifmt, w, h, d);
}
static void my_glCompressedTexImage3D(unsigned tgt, int lvl, unsigned ifmt,
                                      int w, int h, int d, int border,
                                      int sz, const void *px) {
  static void (*real)(unsigned, int, unsigned, int, int, int, int, int,
                      const void *) = NULL;
  rgl("glCompressedTexImage3D", (void **)&real);
  fprintf(stderr, "[TEX3D] glCompressedTexImage3D tgt=0x%x ifmt=0x%x %dx%dx%d\n",
          tgt, ifmt, w, h, d);
  if (real) real(tgt, lvl, ifmt, w, h, d, border, sz, px);
}

/* A engine resolve as funções GL via dlsym DIRETO (libGLESv2 do device),
 * driblando a tabela de imports E o eglGetProcAddress. Interceptamos dlsym
 * p/ devolver NOSSO glGetString (strings curtas) e deixar o resto passar.
 * Oboe também faz dlopen("libOpenSLES.so")+dlsym em runtime (linkOpenSLES) ->
 * roteamos pro opensles_shim (receita do Sonic Mania). */
#define SL_MAGIC ((void *)0x5151ABCDul)
extern uint32_t slCreateEngine_shim(void **, uint32_t, const void *, uint32_t,
                                    const void *, const void *);
static void *sl_dlsym(const char *name) {
  if (!name) return NULL;
  if (strcmp(name, "slCreateEngine") == 0) return (void *)slCreateEngine_shim;
  if (strcmp(name, "SL_IID_ENGINE") == 0) return (void *)&SL_IID_ENGINE_v;
  if (strcmp(name, "SL_IID_PLAY") == 0) return (void *)&SL_IID_PLAY_v;
  if (strcmp(name, "SL_IID_RECORD") == 0) return (void *)&SL_IID_RECORD_v;
  if (strcmp(name, "SL_IID_BUFFERQUEUE") == 0) return (void *)&SL_IID_BUFFERQUEUE_v;
  if (strcmp(name, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE") == 0)
    return (void *)&SL_IID_ANDROIDSIMPLEBUFFERQUEUE_v;
  if (strcmp(name, "SL_IID_ANDROIDCONFIGURATION") == 0)
    return (void *)&SL_IID_ANDROIDCONFIGURATION_v;
  fprintf(stderr, "[sl] dlsym %s -> NULL\n", name);
  return NULL;
}
static void *my_dlsym(void *handle, const char *name) {
  void *ov = coi_gl_proc_override(name);
  if (ov) { fprintf(stderr, "[my_dlsym] override %s\n", name); return ov; }
  if (handle == SL_MAGIC) {
    void *r = sl_dlsym(name);
    fprintf(stderr, "[sl] dlsym %s -> %p\n", name ? name : "?", r);
    return r;
  }
  return dlsym(handle, name);
}
static void *my_dlopen(const char *name, int flag) {
  if (name && strstr(name, "OpenSLES")) {
    /* COI_NOAUDIO=1 -> dlopen falha (NULL) p/ FMOD/engine cair em nosound e
     * seguir pro render (device sem libOpenSLES). Sem a env, usa o shim. */
    if (getenv("COI_NOAUDIO")) {
      fprintf(stderr, "[sl] dlopen %s -> NULL (COI_NOAUDIO)\n", name);
      return NULL;
    }
    fprintf(stderr, "[sl] dlopen %s -> shim\n", name);
    return SL_MAGIC;
  }
  return dlopen(name, flag);
}
/* 🔑 FIX crash de áudio: FMOD_OS_Output_GetDefault faz dlopen("libOpenSLES.so")
 * (recebe nosso SL_MAGIC) e dlclose logo em seguida (só sonda existência).
 * Repassar o handle FAKE pro dlclose real explode no ld-linux (memcpy NULL). */
static int   my_dlclose(void *h) {
  if (h == SL_MAGIC) { fprintf(stderr, "[sl] dlclose(shim) -> 0\n"); return 0; }
  return dlclose(h);
}

/* ---- GL wrappers com LOG (pinpoint do stack-smash no renderer init) ---- */
static void rgl(const char *n, void **slot) {
  if (!*slot) *slot = dlsym(RTLD_DEFAULT, n);
}
static void my_glGetIntegerv(unsigned pname, int *params) {
  static void (*real)(unsigned, int *) = NULL; rgl("glGetIntegerv", (void **)&real);
  /* TRACE POR FRAME — so' com COI_DEBUG=1. A engine chama isto varias
   * vezes por frame; sem o gate, 99% do debug.log virava esta linha (9440 de
   * ~9500 linhas numa corrida de 45s no R36T) e cada frame gerava escrita no
   * cartao SD. Era o pinpoint do stack-smash no init do renderer, ja resolvido. */
  static int trace = -1;
  if (trace < 0) trace = getenv("COI_DEBUG") ? 1 : 0;
  if (trace) fprintf(stderr, "[GL] glGetIntegerv(0x%x)\n", pname);
  /* pnames multi-valor que estouram buffer de 1 int -> zera */
  if (pname == 0x86A3 /*COMPRESSED_TEXTURE_FORMATS*/ ||
      pname == 0x8DF8 /*SHADER_BINARY_FORMATS*/ ||
      pname == 0x87FE /*PROGRAM_BINARY_FORMATS*/) { if (params) params[0] = 0; return; }
  if (real) real(pname, params); else if (params) params[0] = 0;
}
static const unsigned char *my_glGetStringi(unsigned name, unsigned index) {
  fprintf(stderr, "[GL] glGetStringi(0x%x, %u)\n", name, index);
  return (const unsigned char *)"";
}
static unsigned my_glGetError(void) {
  static unsigned (*real)(void) = NULL; rgl("glGetError", (void **)&real);
  return real ? real() : 0;
}
/* diag ArkOS/R36S: "failed to create a vertex shader" — quem falha, create ou erro pendente? */
static unsigned my_glCreateShader(unsigned type) {
  static unsigned (*real)(unsigned) = NULL; rgl("glCreateShader", (void **)&real);
  static unsigned (*ge)(void) = NULL; rgl("glGetError", (void **)&ge);
  unsigned pend = ge ? ge() : 0;
  unsigned r = real ? real(type) : 0;
  unsigned e = ge ? ge() : 0;
  static int n = 0;
  if (n < 12) { fprintf(stderr, "[CREATESHADER] type=0x%x -> id=%u pend=0x%x err=0x%x tid=%d\n",
                        type, r, pend, e, (int)syscall(178)); n++; }
  return r;
}

/* ---- Interceptação de SHADERS (diag do mundo branco) ----
 * Loga source (gated COI_SHADER_DUMP) e SEMPRE loga erro de compile/link.
 * A engine é ES3; shaders #version 300 es podem falhar no Mali GLES2 → render
 * cai p/ fallback branco. */
static int g_shader_dump = -1;
static int shader_dump_on(void) {
  if (g_shader_dump < 0) g_shader_dump = getenv("COI_SHADER_DUMP") ? 1 : 0;
  return g_shader_dump;
}
static void my_glShaderSource(unsigned sh, int count, const char *const *str,
                              const int *len) {
  static void (*real)(unsigned, int, const char *const *, const int *) = NULL;
  rgl("glShaderSource", (void **)&real);
  if (shader_dump_on()) {
    fprintf(stderr, "[SHADER src #%u] count=%d:\n", sh, count);
    for (int i = 0; i < count && i < 8; i++)
      fprintf(stderr, "%.*s", len && len[i] > 0 ? len[i] : 20000, str[i]);
    fprintf(stderr, "\n[/SHADER src #%u]\n", sh);
  }
  store_shader_src(sh, str[0], len ? len[0] : -1);
  /* 🔑 COI FIX Mickey preto (multi-pass): os passes de luz do personagem
   * dependem de Z IDENTICO entre programas (depth LEQUAL repass). O compilador
   * do Mali GP arredonda gl_Position diferente por shader -> passes aditivos
   * reprovam no depth -> so o base ambient (escuro) aparece.
   * "invariant gl_Position;" força Z invariante (fix da spec p/ multi-pass).
   * COI_NO_INVARIANT=1 desliga. */
  /* (s3) DEFAULT OFF: a raiz do Mickey preto era o npot_fix (wrap CLAMP), não Z
   * multi-pass; invariant custa otimização do GP. Religar: COI_INVARIANT=1. */
  if (getenv("COI_INVARIANT") && count == 1 && str[0] &&
      strstr(str[0], "gl_Position") && !strstr(str[0], "invariant")) {
    static char ivb[32768];
    size_t Li = (len && len[0] > 0) ? (size_t)len[0] : strlen(str[0]);
    const char *pre = "invariant gl_Position;\n";
    size_t pl0 = strlen(pre);
    if (Li + pl0 < sizeof(ivb) - 4) {
      memcpy(ivb, pre, pl0);
      memcpy(ivb + pl0, str[0], Li);
      ivb[pl0 + Li] = 0;
      static int ivn = 0;
      if (ivn < 6) { fprintf(stderr, "[INVAR] shader #%u gl_Position invariant\n", sh); ivn++; }
      /* segue o fluxo com o fonte prefixado (inclusive sqrt-abs abaixo) */
      static const char *ps2; static int pl2;
      ps2 = ivb; pl2 = (int)(pl0 + Li);
      str = &ps2; len = &pl2;
    }
  }
  /* 🔑 COI FIX Mickey preto: no Mali-450 (fp reduzido), sqrt(d*d - t*t) do
   * termo cookie/radial das luzes dá NEGATIVO por precisão quando o alvo está
   * quase colinear com a luz (Mickey sob a lanterna do nível 1) -> NaN ->
   * NaN*0=NaN -> pixel PRETO. Embrulhamos TODO argumento de sqrt() em abs()
   * (inofensivo p/ args já positivos). COI_NO_SQRTABS=1 desliga. */
  /* (s3) DEFAULT OFF: NaN de sqrt nunca foi a raiz (Switch/Android rodam sem);
   * evita op extra por sqrt no GP. Religar se piscar preto: COI_SQRTABS=1. */
  if (getenv("COI_SQRTABS") && count == 1 && str[0] && strstr(str[0], "sqrt(")) {
    static char sq[32768];
    size_t L0 = (len && len[0] > 0) ? (size_t)len[0] : strlen(str[0]);
    if (L0 < sizeof(sq) / 2) {
      size_t oi = 0; int wrapped = 0, depth = 0, sp = 0;
      int close_at[64];
      const char *src0 = str[0];
      for (size_t i = 0; i < L0 && oi + 12 < sizeof(sq); i++) {
        if (i + 5 <= L0 && strncmp(src0 + i, "sqrt(", 5) == 0) {
          memcpy(sq + oi, "sqrt(abs(", 9); oi += 9;
          i += 4;
          depth++;
          if (sp < 64) close_at[sp++] = depth;
          wrapped++;
          continue;
        }
        char c = src0[i];
        if (c == '(') depth++;
        if (c == ')') {
          if (sp > 0 && close_at[sp - 1] == depth) { sq[oi++] = ')'; sp--; }
          depth--;
        }
        sq[oi++] = c;
      }
      sq[oi] = 0;
      if (wrapped > 0 && sp == 0) {
        static int sqn = 0;
        if (sqn < 10) { fprintf(stderr, "[SQRTABS] shader #%u: %d sqrt() -> abs\n", sh, wrapped); sqn++; }
        const char *ps = sq; int pl = (int)oi;
        if (real) real(sh, 1, &ps, &pl);
        return;
      }
    }
  }
  /* DIAG: reescreve o fim do fragment shader. RED=vermelho sólido (localiza
   * geometria). TEX=só a textura (sem _vary_color, p/ ver se a cor lava). */
  /* COI_NOCOL: troca "_vary_color * diffuse_sample" por "diffuse_sample"
   * no shader básico — testa se a cor de vértice (luz assada) lava branco. */
  /* COI_PROBE: sondas do Mickey preto (shader skinned+lit do nivel)
   * 1 = selfillum sem a_color (a_color morta?)  2 = int_color = a_color (viz)
   * 3 = int_color = normal*0.5+0.5 (viz da normal skinned) */
  {
    const char *pv = getenv("COI_PROBE");
    if (pv && count == 1 && str[0] && strstr(str[0], "u_bone_matrices") &&
        strstr(str[0], "int_self_illum_color")) {
      static char pb[32768];
      size_t L = (len && len[0] > 0) ? (size_t)len[0] : strlen(str[0]);
      if (L < sizeof(pb) - 256) {
        memcpy(pb, str[0], L); pb[L] = 0;
        char *hit = strstr(pb, "int_self_illum_color = (g_CharacterLightColour.xyz * a_color.yyy);");
        if (hit) {
          const char *rep = NULL;
          if (pv[0] == '1') rep = "int_self_illum_color = (g_CharacterLightColour.xyz * vec3(1.0)  );";
          if (pv[0] == '2') rep = "int_self_illum_color = (a_color.xyz*vec3(4.0)                   );";
          if (rep && strlen(rep) == strlen("int_self_illum_color = (g_CharacterLightColour.xyz * a_color.yyy);")) {
            memcpy(hit, rep, strlen(rep));
            static int pn = 0;
            if (pn < 8) { fprintf(stderr, "[PROBE%c] shader #%u patchado\n", pv[0], sh); pn++; }
            const char *ps = pb; int pl = (int)L;
            if (real) real(sh, 1, &ps, &pl);
            return;
          }
          /* probe 3: int_color constante no fim do VS (varying vs matematica) */
          if (pv[0] == '3') {
            char *ic = strstr(pb, "int_color.xyz = (((");
            if (ic) {
              const char *ins = "int_color.xyz = vec3(0.7);vec3 dumy_c = (((";
              size_t oldpfx = strlen("int_color.xyz = (((");
              size_t newpfx = strlen(ins);
              size_t rest = strlen(ic + oldpfx);
              if (L + (newpfx - oldpfx) < sizeof(pb) - 8) {
                memmove(ic + newpfx, ic + oldpfx, rest + 1);
                memcpy(ic, ins, newpfx);
                static int p3 = 0;
                if (p3 < 8) { fprintf(stderr, "[PROBE3] shader #%u patchado\n", sh); p3++; }
                const char *ps = pb; int pl = (int)strlen(pb);
                if (real) real(sh, 1, &ps, &pl);
                return;
              }
            }
          }
        }
      }
    }
  }
  /* COI_CHARFS: probe decisivo do Mickey/porta preta — visualiza TERMOS da luz
   * no fragment de TODOS os materiais de personagem/objeto dinamico (fragment
   * contem int_self_illum_color; identico em todas as variantes).
   *   light = int_color + int_self_illum (total de luz que chega do VS)
   *   self  = so int_self_illum (charLight * a_color.y)
   *   amb   = so int_color (ambient + luzes dinamicas do VS)
   *   tex   = so texture2D(s_diffuse) (albedo puro) */
  {
    const char *cm = getenv("COI_CHARFS");
    if (cm && count == 1 && str[0] && strstr(str[0], "int_self_illum_color") &&
        strstr(str[0], "gl_FragColor = f_5;")) {
      const char *newl = NULL;
      if (!strcmp(cm, "light")) newl = "gl_FragColor = vec4(int_color.xyz + int_self_illum_color, 1.0);";
      else if (!strcmp(cm, "self")) newl = "gl_FragColor = vec4(int_self_illum_color, 1.0);";
      else if (!strcmp(cm, "amb"))  newl = "gl_FragColor = vec4(int_color.xyz, 1.0);";
      else if (!strcmp(cm, "tex"))  newl = "gl_FragColor = texture2D(s_diffuse, int_uv);";
      else if (!strcmp(cm, "texamp")) newl = "gl_FragColor = texture2D(s_diffuse, int_uv) * 3.0;";
      if (newl) {
        static char cb[32768];
        size_t L = (len && len[0] > 0) ? (size_t)len[0] : strlen(str[0]);
        if (L + strlen(newl) < sizeof(cb) - 8) {
          memcpy(cb, str[0], L); cb[L] = 0;
          char *hit = strstr(cb, "gl_FragColor = f_5;");
          if (hit) {
            size_t oldn = strlen("gl_FragColor = f_5;");
            size_t newn = strlen(newl);
            memmove(hit + newn, hit + oldn, strlen(hit + oldn) + 1);
            memcpy(hit, newl, newn);
            static int cn = 0;
            if (cn < 10) { fprintf(stderr, "[CHARFS:%s] shader #%u patchado\n", cm, sh); cn++; }
            const char *ps = cb; int pl = (int)strlen(cb);
            if (real) real(sh, 1, &ps, &pl);
            return;
          }
        }
      }
    }
  }
  const char *repl = NULL;
  if (getenv("COI_NOCOL")) repl = "(vec4(1.0)) * diffuse_sample"; /* 28, branco */
  else if (getenv("COI_GREEN")) repl = "vec4(0,1,0,1)*diffuse_sample"; /* 28, verde */
  if (count == 1 && repl && str[0] &&
      strstr(str[0], "_vary_color * diffuse_sample")) {
    static char nb[16384];
    size_t L = (len && len[0] > 0) ? (size_t)len[0] : strlen(str[0]);
    if (L < sizeof(nb) - 4) {
      memcpy(nb, str[0], L); nb[L] = 0;
      char *p = strstr(nb, "_vary_color * diffuse_sample");
      if (p) {
        memmove(p, repl, 28);
        const char *pp = nb; int nl = (int)strlen(nb);
        if (real) { real(sh, 1, &pp, &nl); return; }
      }
    }
  }
  const char *inj = NULL;
  if (getenv("COI_SHADER_RED")) inj = "gl_FragColor=vec4(1.0,0.0,0.0,1.0);}";
  else if (getenv("COI_SHADER_TEX")) inj = "gl_FragColor=texture2D(_tex_diffuse,_vary_texture_coordinate);}";
  if (count == 1 && inj) {
    const char *s = str[0];
    if (s && strstr(s, "gl_FragColor") && strstr(s, "_tex_diffuse") &&
        strstr(s, "_vary_texture_coordinate")) {
      static char nb[16384];
      size_t L = (len && len[0] > 0) ? (size_t)len[0] : strlen(s);
      if (L < sizeof(nb) - 80) {
        memcpy(nb, s, L); nb[L] = 0;
        char *last = strrchr(nb, '}');
        if (last) {
          strcpy(last, inj);
          const char *p = nb; int nl = (int)strlen(nb);
          if (real) real(sh, 1, &p, &nl);
          return;
        }
      }
    }
  }
  if (real) real(sh, count, str, len);
}
static void my_glCompileShader(unsigned sh) {
  static void (*real)(unsigned) = NULL; rgl("glCompileShader", (void **)&real);
  static void (*giv)(unsigned, unsigned, int *) = NULL;
  rgl("glGetShaderiv", (void **)&giv);
  static void (*glog)(unsigned, int, int *, char *) = NULL;
  rgl("glGetShaderInfoLog", (void **)&glog);
  if (real) real(sh);
  if (giv && glog) {
    int ok = 1; giv(sh, 0x8B81 /*COMPILE_STATUS*/, &ok);
    if (!ok) {
      char buf[1024]; int n = 0; glog(sh, sizeof(buf) - 1, &n, buf);
      buf[n > 0 ? n : 0] = 0;
      fprintf(stderr, "[SHADER #%u COMPILE FALHOU] %s\n", sh, buf);
    }
  }
}
/* armazena source de cada shader (p/ dumpar o do program do chão) */
static struct { unsigned sh; char *src; } g_shsrc[256]; static int g_nsh = 0;
static void store_shader_src(unsigned sh, const char *s, int len) {
  if (g_nsh >= 256) return;
  int L = len > 0 ? len : (int)strlen(s);
  char *c = malloc(L + 1); if (!c) return;
  memcpy(c, s, L); c[L] = 0;
  g_shsrc[g_nsh].sh = sh; g_shsrc[g_nsh].src = c; g_nsh++;
}
static void my_glLinkProgram(unsigned pr) {
  static void (*real)(unsigned) = NULL; rgl("glLinkProgram", (void **)&real);
  /* dump da fragment source do programa alvo (COI_DUMP_PROG=N) */
  const char *want = getenv("COI_DUMP_PROG");
  if (want && (unsigned)atoi(want) == pr) {
    static void (*gas)(unsigned,int,int*,unsigned*) = NULL;
    rgl("glGetAttachedShaders", (void **)&gas);
    if (gas) {
      unsigned shs[8]; int cnt = 0; gas(pr, 8, &cnt, shs);
      for (int i = 0; i < cnt; i++)
        for (int j = 0; j < g_nsh; j++)
          if (g_shsrc[j].sh == shs[i] && strstr(g_shsrc[j].src, "gl_FragColor"))
            fprintf(stderr, "[PROGSRC prog=%u sh=%u]\n%s\n[/PROGSRC]\n", pr, shs[i], g_shsrc[j].src);
    }
  }
  static void (*giv)(unsigned, unsigned, int *) = NULL;
  rgl("glGetProgramiv", (void **)&giv);
  static void (*plog)(unsigned, int, int *, char *) = NULL;
  rgl("glGetProgramInfoLog", (void **)&plog);
  if (real) real(pr);
  if (giv && plog) {
    int ok = 1; giv(pr, 0x8B82 /*LINK_STATUS*/, &ok);
    { /* par VS/FS de cada programa (achar o FS REAL do Mickey) */
      static void (*gas)(unsigned, int, int *, unsigned *) = NULL;
      rgl("glGetAttachedShaders", (void **)&gas);
      if (gas && getenv("COI_UNIF_LOG")) {
        unsigned shs[4] = {0, 0, 0, 0};
        int n2 = 0;
        gas(pr, 4, &n2, shs);
        static int lp = 0;
        if (lp < 400) {
          fprintf(stderr, "[LINKPAIR] prog=%u shaders=%u,%u\n", pr, shs[0], shs[1]);
          lp++;
        }
      }
    }
    /* COI diag: locations REAIS dos elementos de array de luz no Mali
     * (engine assume base+i — spec NAO garante; Mali pode divergir) */
    if (getenv("COI_UNIF_LOG")) {
      static int an = 0;
      static int (*gul)(unsigned, const char *) = NULL;
      rgl("glGetUniformLocation", (void **)&gul);
      if (gul && an < 6) {
        int b = gul(pr, "u_bone_matrices");
        if (b >= 0) { /* so os programas skinned */
          an++;
          const char *nm[] = {"lightsCol_Dir[0]","lightsCol_Dir[1]",
            "lightsPos_Dir[0]","lightsPos_Dir[1]","lightsAtt_Dir[0]",
            "lightsDat_Dir[0]","lightsCol_Omni[0]","lightsPos_Omni[0]",
            "lightsAtt_Omni[0]","g_CharacterLightColour","g_Ambient",
            "u_bone_matrices[0]","u_bone_matrices[1]","u_bone_matrices[89]",
            "lightsCol_Dir","lightsPos_Dir","g_CamDir","g_mWorld"};
          for (unsigned q = 0; q < sizeof(nm)/sizeof(nm[0]); q++)
            fprintf(stderr, "[ARRLOC] prog=%u %s -> %d\n", pr, nm[q], gul(pr, nm[q]));
        }
      }
    }

    if (!ok) {
      char buf[1024]; int n = 0; plog(pr, sizeof(buf) - 1, &n, buf);
      buf[n > 0 ? n : 0] = 0;
      fprintf(stderr, "[PROGRAM #%u LINK FALHOU] %s\n", pr, buf);
    }
  }
}

/* __stack_chk_fail neutralizado: a stack-canary da engine (bionic) é lida de
 * tpidr_el0+0x28, que sob glibc colide com TLS vars nossas/do libc++ -> a canary
 * "muda" no meio da função = FALSO-POSITIVO. Em vez de abortar, retornamos -> a
 * função segue o ret normal. (O guard do egl_shim já foi estabilizado tirando
 * _Thread_local; isto cobre os demais paths.) */
static void my_stack_chk_fail(void) {
  static int n = 0;
  if (n++ < 3) fprintf(stderr, "[stack_chk_fail] FALSO-POSITIVO TLS ignorado\n");
}

/* GUARDA no memcpy: o caminho skinned-actor (StageImporter::AddActorFromNode →
 * ModelInstance::InitializeFromModel) crasha com memcpy(dst=NULL, n=11) ao
 * importar o mapa (config change). Se dst/src inválidos, loga o caller e PULA
 * (em vez de SIGSEGV) → o loading do mundo continua. */
static int copy_bad(const char *who, void *dst, const void *src, size_t n) {
  if ((uintptr_t)dst >= 0x10000 && (uintptr_t)src >= 0x10000) return 0;
  static int g = 0;
  if (g < 30) { fprintf(stderr, "[%s-GUARD] dst=%p src=%p n=%zu — PULANDO\n", who, dst, src, n); g++; }
  return 1;
}
static void *my_memcpy(void *dst, const void *src, size_t n) {
  static void *(*real)(void *, const void *, size_t) = NULL;
  if (!real) real = dlsym(RTLD_DEFAULT, "memcpy");
  if (copy_bad("memcpy", dst, src, n)) return dst;
  return real(dst, src, n);
}
static void *my_memmove(void *dst, const void *src, size_t n) {
  static void *(*real)(void *, const void *, size_t) = NULL;
  if (!real) real = dlsym(RTLD_DEFAULT, "memmove");
  if (copy_bad("memmove", dst, src, n)) return dst;
  return real(dst, src, n);
}
static void *my_memcpy_chk(void *dst, const void *src, size_t n, size_t dl) {
  static void *(*real)(void *, const void *, size_t) = NULL;
  if (!real) real = dlsym(RTLD_DEFAULT, "memcpy");
  (void)dl; if (copy_bad("memcpy_chk", dst, src, n)) return dst;
  return real(dst, src, n);
}
static void *my_memmove_chk(void *dst, const void *src, size_t n, size_t dl) {
  static void *(*real)(void *, const void *, size_t) = NULL;
  if (!real) real = dlsym(RTLD_DEFAULT, "memmove");
  (void)dl; if (copy_bad("memmove_chk", dst, src, n)) return dst;
  return real(dst, src, n);
}

DynLibFunction coi_overrides[] = {
  /* liblog */
  {"__android_log_print", (uintptr_t)b_log_print},
  {"__android_log_write", (uintptr_t)b_log_write},
  {"__android_log_assert", (uintptr_t)b_log_assert},
  /* bionic stdio __sF + wrappers (resolve UNRESOLVED do libc++ -> std::cerr) */
  {"__sF", (uintptr_t)bionic_sF},
  {"android_set_abort_message", (uintptr_t)b_set_abort_message},
  {"fprintf", (uintptr_t)w_fprintf}, {"vfprintf", (uintptr_t)w_vfprintf},
  {"fwrite", (uintptr_t)w_fwrite}, {"fputs", (uintptr_t)w_fputs},
  {"fputc", (uintptr_t)w_fputc}, {"fflush", (uintptr_t)w_fflush},
  /* bionic libc */
  {"__errno", (uintptr_t)b_errno},
  {"setjmp", (uintptr_t)_setjmp},
  {"longjmp", (uintptr_t)_longjmp},
  {"__assert2", (uintptr_t)b_assert2},
  {"__strlen_chk", (uintptr_t)b_strlen_chk},
  {"__strchr_chk", (uintptr_t)b_strchr_chk},
  {"__umask_chk", (uintptr_t)b_umask_chk},
  {"__system_property_get", (uintptr_t)b_sys_prop_get},
  {"__emutls_get_address", (uintptr_t)__emutls_get_address},
  /* EGL -> egl_shim */
  {"eglGetDisplay", (uintptr_t)egl_shim_GetDisplay},
  {"eglInitialize", (uintptr_t)egl_shim_Initialize},
  {"eglTerminate", (uintptr_t)egl_shim_Terminate},
  {"eglChooseConfig", (uintptr_t)egl_shim_ChooseConfig},
  {"eglCreateWindowSurface", (uintptr_t)egl_shim_CreateWindowSurface},
  {"eglCreateContext", (uintptr_t)egl_shim_CreateContext},
  {"eglDestroyContext", (uintptr_t)egl_shim_DestroyContext},
  {"eglDestroySurface", (uintptr_t)egl_shim_DestroySurface},
  {"eglGetConfigAttrib", (uintptr_t)egl_shim_GetConfigAttrib},
  {"eglGetError", (uintptr_t)egl_shim_GetError},
  {"eglGetProcAddress", (uintptr_t)egl_shim_GetProcAddress},
  {"eglMakeCurrent", (uintptr_t)egl_shim_MakeCurrent},
  {"eglSwapBuffers", (uintptr_t)egl_shim_SwapBuffers},
  {"eglSwapInterval", (uintptr_t)egl_shim_SwapInterval},
  {"eglBindAPI", (uintptr_t)egl_shim_BindAPI},
  {"eglReleaseThread", (uintptr_t)egl_releasethread_stub},
  /* GL string override (anti stack-smash do Utgard) */
  {"glGetString", (uintptr_t)my_glGetString},
  /* intercepta dlsym (a engine resolve GL por aqui) */
  {"dlsym", (uintptr_t)my_dlsym},
  {"dlopen", (uintptr_t)my_dlopen},
  {"dlclose", (uintptr_t)my_dlclose},
  /* GL wrappers com log p/ pinpoint do smash */
  {"glGetIntegerv", (uintptr_t)my_glGetIntegerv},
  {"glGetStringi", (uintptr_t)my_glGetStringi},
  {"glGetError", (uintptr_t)my_glGetError},
  /* GL core interceptados p/ diag/fix do mundo branco (resolvidos pela tabela,
   * NÃO via eglGetProcAddress) */
  {"glTexImage2D", (uintptr_t)my_glTexImage2D},
  {"glCreateShader", (uintptr_t)my_glCreateShader},
  /* DYS_PAGE: atlas dinamico + limpeza de estado por id */
  {"glTexSubImage2D", (uintptr_t)my_glTexSubImage2D},
  {"glCopyTexSubImage2D", (uintptr_t)my_glCopyTexSubImage2D},
  {"glDeleteTextures", (uintptr_t)my_glDeleteTextures},
  {"glShaderSource", (uintptr_t)my_glShaderSource},
  {"glCompileShader", (uintptr_t)my_glCompileShader},
  {"glLinkProgram", (uintptr_t)my_glLinkProgram},
  {"glCompressedTexImage2D", (uintptr_t)my_glCompressedTexImage2D},
  {"glTexParameteri", (uintptr_t)my_glTexParameteri},
  {"glClearColor", (uintptr_t)my_glClearColor},
  {"glBlendFunc", (uintptr_t)my_glBlendFunc},
  {"glBlendFuncSeparate", (uintptr_t)my_glBlendFuncSeparate},
  {"glBindBuffer", (uintptr_t)my_glBindBuffer},
  {"glUseProgram", (uintptr_t)my_glUseProgram},
  {"glBufferData", (uintptr_t)my_glBufferData},
  {"glDeleteBuffers", (uintptr_t)my_glDeleteBuffers},
  {"glEnable", (uintptr_t)my_glEnable},
  {"glDisable", (uintptr_t)my_glDisable},
  {"glDepthMask", (uintptr_t)my_glDepthMask},
  {"glColorMask", (uintptr_t)my_glColorMask},
  {"glDepthFunc", (uintptr_t)my_glDepthFunc},
  {"glGetUniformLocation", (uintptr_t)my_glGetUniformLocation},
  {"glUniform1i", (uintptr_t)my_glUniform1i},
  {"glUniform4fv", (uintptr_t)my_glUniform4fv},
  {"glUniform3fv", (uintptr_t)my_glUniform3fv},
  {"glUniform1f", (uintptr_t)my_glUniform1f},
  {"glUniform4i", (uintptr_t)my_glUniform4i},
  {"glUniformMatrix4fv", (uintptr_t)my_glUniformMatrix4fv},
  {"glBindAttribLocation", (uintptr_t)my_glBindAttribLocation},
  {"glBindTexture", (uintptr_t)my_glBindTexture},
  {"glActiveTexture", (uintptr_t)my_glActiveTexture},
  {"glGetAttribLocation", (uintptr_t)my_glGetAttribLocation},
  {"glVertexAttribPointer", (uintptr_t)my_glVertexAttribPointer},
  {"glEnableVertexAttribArray", (uintptr_t)my_glEnableVertexAttribArray},
  {"glDisableVertexAttribArray", (uintptr_t)my_glDisableVertexAttribArray},
  {"glDrawArrays", (uintptr_t)my_glDrawArrays},
  {"glDrawElements", (uintptr_t)my_glDrawElements},
  {"glBindFramebuffer", (uintptr_t)my_glBindFramebuffer},
  {"glCheckFramebufferStatus", (uintptr_t)my_glCheckFramebufferStatus},
  {"glRenderbufferStorage", (uintptr_t)my_glRenderbufferStorage},
  {"glViewport", (uintptr_t)my_glViewport},  /* T2: resolução interna */
  {"glFramebufferTexture2D", (uintptr_t)my_glFramebufferTexture2D},
  {"pthread_attr_setstacksize", (uintptr_t)my_attr_setstacksize},
  /* glibc <=2.32 (ArkOS 2.30) NAO exporta stat/lstat/fstat da libc.so (sao do
   * libc_nonshared.a) -> dlsym falha -> slot com lixo -> SIGSEGV no 1o stat da
   * engine (open do gamedata/.log). Apontar pros simbolos LOCAIS do binario
   * (aarch64: struct stat bionic == kernel == glibc; receita do chrono). */
  {"stat", (uintptr_t)&stat},
  {"lstat", (uintptr_t)&lstat},
  {"fstat", (uintptr_t)&fstat},
  {"fopen", (uintptr_t)my_fopen},
  {"__stack_chk_fail", (uintptr_t)my_stack_chk_fail},
  {"memcpy", (uintptr_t)my_memcpy},
  {"memmove", (uintptr_t)my_memmove},
  {"__memcpy_chk", (uintptr_t)my_memcpy_chk},
  {"__memmove_chk", (uintptr_t)my_memmove_chk},
  /* ANativeWindow */
  {"ANativeWindow_fromSurface", (uintptr_t)aw_fromSurface},
  {"ANativeWindow_acquire", (uintptr_t)aw_acquire},
  {"ANativeWindow_release", (uintptr_t)aw_release},
  {"ANativeWindow_getWidth", (uintptr_t)aw_getWidth},
  {"ANativeWindow_getHeight", (uintptr_t)aw_getHeight},
  /* AAsset */
  {"AAssetManager_fromJava", (uintptr_t)aam_fromJava},
  {"AAssetManager_open", (uintptr_t)aam_open},
  {"AAssetManager_openDir", (uintptr_t)aam_openDir},
  {"AAsset_read", (uintptr_t)aa_read},
  {"AAsset_seek", (uintptr_t)aa_seek},
  {"AAsset_seek64", (uintptr_t)aa_seek64},
  {"AAsset_getLength", (uintptr_t)aa_getLength},
  {"AAsset_getLength64", (uintptr_t)aa_getLength},
  {"AAsset_getRemainingLength", (uintptr_t)aa_getRemaining},
  {"AAsset_getRemainingLength64", (uintptr_t)aa_getRemaining},
  {"AAsset_close", (uintptr_t)aa_close},
  {"AAsset_openFileDescriptor", (uintptr_t)aa_openFd},
  {"AAssetDir_getNextFileName", (uintptr_t)aad_getNext},
  {"AAssetDir_close", (uintptr_t)aad_close},
  /* ALooper extras */
  {"ALooper_pollOnce", (uintptr_t)al_pollOnce},
  {"ALooper_forThread", (uintptr_t)al_forThread},
  {"ALooper_acquire", (uintptr_t)al_acquire},
  {"ALooper_release", (uintptr_t)al_release},
  {"ALooper_removeFd", (uintptr_t)al_removeFd},
  {"ALooper_wake", (uintptr_t)al_wake},
  /* AInput extras */
  {"AInputEvent_getDeviceId", (uintptr_t)aie_getDeviceId},
  {"AInputQueue_hasEvents", (uintptr_t)AInputQueue_hasEvents},
  {"AMotionEvent_getButtonState", (uintptr_t)ame_getButtonState},
  {"AMotionEvent_getFlags", (uintptr_t)AMotionEvent_getFlags},
  /* OpenSL */
  {"slCreateEngine", (uintptr_t)slCreateEngine_shim},
  {"SL_IID_ENGINE", (uintptr_t)&SL_IID_ENGINE_v},
  {"SL_IID_PLAY", (uintptr_t)&SL_IID_PLAY_v},
  {"SL_IID_RECORD", (uintptr_t)&SL_IID_RECORD_v},
  {"SL_IID_BUFFERQUEUE", (uintptr_t)&SL_IID_BUFFERQUEUE_v},
  {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&SL_IID_ANDROIDSIMPLEBUFFERQUEUE_v},
  {"SL_IID_ANDROIDCONFIGURATION", (uintptr_t)&SL_IID_ANDROIDCONFIGURATION_v},
};
const int coi_overrides_count =
    sizeof(coi_overrides) / sizeof(coi_overrides[0]);
