/* imports.c (DEVICE) -- SHANTAE: Risky's Revenge / Pirate's Curse
 * (WayForward "Black" engine, NativeActivity native armv7, ES2 + OpenSLES).
 * so-loader p/ NextOS armv7 + Mali-450 (Utgard, GLES2 via SDL2).
 *
 * Tabela de OVERRIDES (resolvida ANTES do fallback dlsym do so_resolve). Tudo
 * que NÃO está aqui cai no dlsym(RTLD_DEFAULT) -> libc/libm/libGLESv2/libEGL/
 * SDL2 pré-carregadas RTLD_GLOBAL.
 *   EGL          -> egl_shim_*   (contexto GLES2 via SDL2, Mali fbdev)
 *   ANativeWindow/AAsset/ALooper extras -> impls locais
 *   OpenSL ES    -> opensles_shim
 *   bionic-only (strlcpy/strlcat/__assert2/log/property) -> wrappers
 *   pthread      -> revc_pthread_table (pthread_bridge.c)
 *
 * REUSA os shims genéricos PROVADOS (Dysmantle/Crazy Taxi verdes); SEM hooks de
 * textura específicos de outro jogo.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "so_util.h"
#include "egl_shim.h"
#include "android_shim.h"

/* ---------------- liblog ---------------- */
extern volatile int sonic_game_started;
static int sonic_env_on(const char *name) {
  const char *v = getenv(name);
  return v && *v && strcmp(v, "0") != 0 && strcasecmp(v, "false") != 0 &&
         strcasecmp(v, "no") != 0 && strcasecmp(v, "off") != 0;
}

static int sonic_verbose_log(void) {
  static int enabled = -1;
  if (enabled < 0)
    enabled = sonic_env_on("SONIC_VERBOSE_LOG") || sonic_env_on("SONIC_ALOG");
  return enabled;
}

static int sonic_memcpy_log(void) {
  static int enabled = -1;
  if (enabled < 0)
    enabled = sonic_env_on("SONIC_VERBOSE_LOG") || sonic_env_on("SONIC_MEMCPYLOG");
  return enabled;
}

static void sonic_update_gameplay_state_from_log(const char *msg) {
  if (!msg) return;

  if (strstr(msg, "Create World Map")) {
    if (sonic_game_started)
      fprintf(stderr, "=== gameplay state: world map/menu ===\n");
    sonic_game_started = 0;
    return;
  }

  /* Special Stage: carrega por CSSLoadingTask/GmSpStage_Start (player GmPlayerSpStage_*),
     caminho SEPARADO do mapfar/GmGameDatLoadExit dos atos normais -> a heuristica abaixo
     nunca dispara e sonic_game_started fica preso em 0 (vindo de "Create World Map").
     Resultado: A vira FOX_A_MENU(0x8020); o bit 0x8000 == OUYAGetPauseKey(), e TODO check
     de pausa faz (pad & 0xC000) -> PULO PAUSA so no special stage.
     Os assets do sp-stage logam "--- Load Start <...SpStage0X...> ---" pelo MESMO logger
     foxLog ja confirmado em runtime; "SPSTAGE BRANCH"/"SpStage Loading" sao o state/loader.
     strcasestr "spstage" casa SpStage01..08 / SPSTAGE BRANCH / SpStage Loading, mas NAO a
     mensagem de UI "Special Stage" (tem espaco). */
  if (strcasestr(msg, "spstage")) {
    if (!sonic_game_started)
      fprintf(stderr, "=== gameplay state: started (special stage: %s) ===\n", msg);
    sonic_game_started = 1;
    return;
  }

  if (strstr(msg, "game start") ||
      strstr(msg, "GmGameDatLoadExit") ||
      strstr(msg, "Gimmick set camera scale") ||
      strstr(msg, "GmPlySeq")) {
    if (!sonic_game_started)
      fprintf(stderr, "=== gameplay state: started (%s) ===\n", msg);
    sonic_game_started = 1;
  }
}

static int b_log_print(int prio, const char *tag, const char *fmt, ...) {
  char msg[1024] = {0};
  va_list ap; va_start(ap, fmt);
  if (fmt)
    vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  if (fmt)
    sonic_update_gameplay_state_from_log(msg);
  if (sonic_verbose_log()) {
    fprintf(stderr, "[ALOG:%d %s] %s\n", prio, tag ? tag : "?", msg);
  }
  return 0;
}
static int b_log_write(int prio, const char *tag, const char *text) {
  sonic_update_gameplay_state_from_log(text);
  if (sonic_verbose_log())
    fprintf(stderr, "[ALOG:%d %s] %s\n", prio, tag ? tag : "?", text ? text : "");
  return 0;
}
static void b_log_assert(const char *cond, const char *tag, const char *fmt, ...) {
  fprintf(stderr, "[ALOG-ASSERT %s] cond=%s ", tag ? tag : "?", cond ? cond : "?");
  if (fmt) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); }
  fprintf(stderr, "\n");
}
static int b_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
  char msg[1024] = {0};
  va_list cp; va_copy(cp, ap);
  if (fmt)
    vsnprintf(msg, sizeof(msg), fmt, cp);
  va_end(cp);
  if (fmt)
    sonic_update_gameplay_state_from_log(msg);
  if (sonic_verbose_log())
    fprintf(stderr, "[ALOG:%d %s] %s\n", prio, tag ? tag : "?", msg);
  return 0;
}

/* ---------------- bionic-only libc ---------------- */
/* glibc não tem strlcpy/strlcat (BSD/bionic). Implementação canônica. */
static size_t b_strlcpy(char *dst, const char *src, size_t dsize) {
  const char *osrc = src; size_t nleft = dsize;
  if (nleft != 0) { while (--nleft != 0) { if ((*dst++ = *src++) == '\0') break; } }
  if (nleft == 0) { if (dsize != 0) *dst = '\0'; while (*src++) ; }
  return src - osrc - 1;
}
static size_t b_strlcat(char *dst, const char *src, size_t dsize) {
  const char *odst = dst; const char *osrc = src; size_t n = dsize; size_t dlen;
  while (n-- != 0 && *dst != '\0') dst++;
  dlen = dst - odst; n = dsize - dlen;
  if (n-- == 0) return dlen + strlen(src);
  while (*src != '\0') { if (n != 0) { *dst++ = *src; n--; } src++; }
  *dst = '\0';
  return dlen + (src - osrc);
}
static void b_assert2(const char *file, int line, const char *func, const char *msg) {
  fprintf(stderr, "[bionic __assert2] %s:%d %s: %s\n",
          file ? file : "?", line, func ? func : "?", msg ? msg : "?");
  abort();
}
static int b_system_property_get(const char *name, char *value) {
  (void)name; if (value) value[0] = '\0'; return 0;
}
/* glibc <= 2.32 (ArkOS 2.30) does not export the plain stat/lstat/fstat
 * symbols that newer glibc has. libfox imports those names directly, so
 * fallback dlsym leaves them unresolved and ArkOS stalls during early file I/O.
 * Use raw arm64 syscalls; the kernel stat layout matches bionic/glibc here. */
static int my_stat(const char *path, void *buf) {
  return syscall(SYS_newfstatat, AT_FDCWD, path, buf, 0);
}
static int my_lstat(const char *path, void *buf) {
  return syscall(SYS_newfstatat, AT_FDCWD, path, buf, AT_SYMLINK_NOFOLLOW);
}
static int my_fstat(int fd, void *buf) {
  return syscall(SYS_fstat, fd, buf);
}
/* bionic usa __errno() (retorna int*); glibc usa __errno_location */
static int *b_errno(void) {
  static int *(*real)(void) = NULL;
  if (!real) real = (int *(*)(void))dlsym(RTLD_DEFAULT, "__errno_location");
  return real ? real() : NULL;
}
/* __stack_chk_fail: no-op (canary bionic tpidr+offset não casa sob glibc) */
static void my_stack_chk_fail(void) { /* no-op; ver nota canary no main */ }

/* ---------------- ANativeWindow ---------------- */
extern int sonic_screen_w, sonic_screen_h; /* resolucao real do egl_shim */
static ANativeWindow *aw_fromSurface(void *env, void *surface) {
  (void)env; (void)surface; return android_shim_get_window();
}
static void aw_acquire(void *w) { (void)w; }
static void aw_release(void *w) { (void)w; }
static int  aw_getWidth(void *w)  { (void)w; return sonic_screen_w; }
static int  aw_getHeight(void *w) { (void)w; return sonic_screen_h; }
static int  aw_getFormat(void *w) { (void)w; return 1; /* WINDOW_FORMAT_RGBA_8888 */ }
static int  aw_setBuffersGeometry(void *w, int x, int y, int f) {
  (void)w; (void)x; (void)y; (void)f; return 0;
}

/* ---------------- AAsset / AAssetManager / AAssetDir ----------------
 * O jogo lê assets/*.vol via AAssetManager. Servimos de um diretório real no
 * device (extraído do APK). Base por env SONIC_ASSETS, default "assets". */
typedef struct { FILE *fp; long len; char path[640]; } ShAsset;
typedef struct { DIR *d; char base[640]; struct dirent *cur; } ShAssetDir;

static const char *assets_base(void) {
  const char *b = getenv("SONIC_ASSETS");
  return (b && *b) ? b : "assets";
}
static void *aam_fromJava(void *env, void *obj) { (void)env; (void)obj; return (void *)1; }
static void *aam_open(void *mgr, const char *fn, int mode) {
  (void)mgr; (void)mode;
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", assets_base(), fn);
  FILE *fp = fopen(path, "rb");
  if (!fp) { fprintf(stderr, "[AAsset] MISS %s\n", path); return NULL; }
  ShAsset *a = calloc(1, sizeof(ShAsset));
  a->fp = fp; fseek(fp, 0, SEEK_END); a->len = ftell(fp); fseek(fp, 0, SEEK_SET);
  snprintf(a->path, sizeof(a->path), "%s", path);
  return a;
}
static int  aa_read(void *h, void *buf, size_t n) {
  ShAsset *a = h; if (!a) return -1; return (int)fread(buf, 1, n, a->fp);
}
static long aa_seek(void *h, long off, int wh) {
  ShAsset *a = h; if (!a) return -1; fseek(a->fp, off, wh); return ftell(a->fp);
}
static long aa_seek64(void *h, long off, int wh) { return aa_seek(h, off, wh); }
static long aa_getLength(void *h)   { ShAsset *a = h; return a ? a->len : 0; }
static long aa_getRemaining(void *h){ ShAsset *a = h; return a ? a->len - ftell(a->fp) : 0; }
static void aa_close(void *h)       { ShAsset *a = h; if (a) { fclose(a->fp); free(a); } }
static int  aa_openFd(void *h, long *start, long *len) {
  ShAsset *a = h; if (!a) return -1;
  if (start) *start = 0; if (len) *len = a->len;
  fflush(a->fp); return dup(fileno(a->fp));
}
static void *aam_openDir(void *mgr, const char *dirn) {
  (void)mgr;
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", assets_base(), dirn ? dirn : "");
  DIR *d = opendir(path);
  if (!d) return NULL;
  ShAssetDir *ad = calloc(1, sizeof(ShAssetDir));
  ad->d = d; snprintf(ad->base, sizeof(ad->base), "%s", path);
  return ad;
}
static const char *aad_getNext(void *h) {
  ShAssetDir *ad = h; if (!ad) return NULL;
  struct dirent *e;
  while ((e = readdir(ad->d))) {
    if (e->d_name[0] == '.') continue;
    return e->d_name;
  }
  return NULL;
}
static void aad_close(void *h) { ShAssetDir *ad = h; if (ad) { closedir(ad->d); free(ad); } }

/* ---------------- ALooper extras ---------------- */
static int  al_pollOnce(int t, int *fd, int *ev, void **data) { return ALooper_pollAll(t, fd, ev, data); }
static void *al_forThread(void) { return (void *)1; }
static void al_acquire(void *l) { (void)l; }
static void al_release(void *l) { (void)l; }
static void al_removeFd(void *l, int fd) { (void)l; (void)fd; }
static void al_wake(void *l) { (void)l; }

/* ---------------- AInput extras ---------------- */
static int  aie_getDeviceId(void *e) { (void)e; return 0; }
static int  ame_getButtonState(void *e) { (void)e; return 0; }

/* ---------------- pthread attr (stack size grande p/ bionic 1MB+) ---------------- */
static int my_attr_setstacksize(void *attr, size_t sz) {
  static int (*real)(void *, size_t) = NULL;
  if (!real) real = (int (*)(void *, size_t))dlsym(RTLD_DEFAULT, "pthread_attr_setstacksize");
  if (sz < 512 * 1024) sz = 512 * 1024;
  return real ? real(attr, sz) : 0;
}

/* ---------------- OpenSL ES interface IDs ---------------- */
extern uint32_t slCreateEngine_shim(void **, uint32_t, const void *, uint32_t,
                                    const void *, const void *);
extern const void *sl_IID_ENGINE, *sl_IID_PLAY, *sl_IID_VOLUME,
    *sl_IID_BUFFERQUEUE;
static const void *SL_IID_ENGINE_v, *SL_IID_PLAY_v, *SL_IID_RECORD_v,
    *SL_IID_VOLUME_v, *SL_IID_BUFFERQUEUE_v, *SL_IID_ANDROIDSIMPLEBUFFERQUEUE_v,
    *SL_IID_ANDROIDCONFIGURATION_v = "ACFG";
__attribute__((constructor)) static void sl_iid_init(void) {
  SL_IID_ENGINE_v = sl_IID_ENGINE;
  SL_IID_PLAY_v = sl_IID_PLAY;
  SL_IID_VOLUME_v = sl_IID_VOLUME;
  SL_IID_RECORD_v = "RECORD";
  SL_IID_BUFFERQUEUE_v = sl_IID_BUFFERQUEUE;
  SL_IID_ANDROIDSIMPLEBUFFERQUEUE_v = sl_IID_BUFFERQUEUE;
}

/* ponte stdio (stdio_shim.c) */
extern size_t b_fwrite(const void *, size_t, size_t, void *);
extern void *b_fopen(const char *, const char *);
extern size_t b_fread(void *, size_t, size_t, void *);
extern int b_fputs(const char *, void *);
extern int b_fputc(int, void *);
extern int b_fgetc(void *);
extern int b_fseek(void *, long, int);
extern long b_ftell(void *);
extern int b_fflush(void *);
extern int b_fclose(void *);

/* memcpy/memmove com guarda de diagnóstico: loga e protege dest/src nulos.
 * (O jogo crasha cedo em memcpy(dest=0,...,11) numa op de std::string na init de
 * idioma; queremos ver a origem e sobreviver p/ avaliar.) */
static void *my_memcpy(void *d, const void *s, size_t n) {
  if ((uintptr_t)d < 0x1000 || (uintptr_t)s < 0x1000) {
    static int z = 0;
    if (sonic_memcpy_log() && z < 40) {
      fprintf(stderr, "[MEMCPY-NULL] dest=%p src=%p n=%zu ret=%p\n",
              d, s, n, __builtin_return_address(0));
      z++;
    }
    return d; /* sobrevive (não copia) */
  }
  return memcpy(d, s, n);
}
static void *my_memmove(void *d, const void *s, size_t n) {
  if ((uintptr_t)d < 0x1000 || (uintptr_t)s < 0x1000) return d;
  return memmove(d, s, n);
}
static void *my_memcpy_chk(void *d, const void *s, size_t n, size_t dn) {
  (void)dn; return my_memcpy(d, s, n);
}
/* __aeabi_memcpy: void(void*,const void*,size_t) — mesmos args do memcpy */
static void my_aeabi_memcpy(void *d, const void *s, size_t n) { my_memcpy(d, s, n); }
static void my_aeabi_memmove(void *d, const void *s, size_t n) { my_memmove(d, s, n); }
/* __aeabi_memset: void(void*, size_t n, int c) — ORDEM DIFERENTE do memset */
static void my_aeabi_memset(void *d, size_t n, int c) {
  if ((uintptr_t)d < 0x1000) return;
  memset(d, c, n);
}

static unsigned egl_releasethread_stub(void) { return 1u; }

/* ---- GL logging (diagnóstico tela preta) — gated por SONIC_GLLOG ---- */
static int g_gllog = -1;
static int gllog_on(void) { if (g_gllog<0) g_gllog = getenv("SONIC_GLLOG")?1:0; return g_gllog; }
static void *rgl(const char *n) { return dlsym(RTLD_DEFAULT, n); }
extern volatile unsigned long sonic_frame_for_imports;
static int g_drawlog = -1, g_force3d = -1, g_force_cull = -1, g_force_depth = -1,
           g_force_scissor = -1, g_force_mask = -1;
static int drawlog_on(void) {
  if (g_drawlog < 0) g_drawlog = getenv("SONIC_DRAWLOG") ? 1 : 0;
  return g_drawlog;
}
static int force3d_on(void) {
  if (g_force3d < 0) g_force3d = getenv("SONIC_FORCE3D_STATE") ? 1 : 0;
  return g_force3d;
}
static int force_cull_on(void) {
  if (g_force_cull < 0) g_force_cull = getenv("SONIC_FORCE3D_CULL") ? 1 : 0;
  return force3d_on() || g_force_cull;
}
static int force_depth_on(void) {
  if (g_force_depth < 0) g_force_depth = getenv("SONIC_FORCE3D_DEPTH") ? 1 : 0;
  return force3d_on() || g_force_depth;
}
static int force_scissor_on(void) {
  if (g_force_scissor < 0) g_force_scissor = getenv("SONIC_FORCE3D_SCISSOR") ? 1 : 0;
  return force3d_on() || g_force_scissor;
}
static int force_mask_on(void) {
  if (g_force_mask < 0) g_force_mask = getenv("SONIC_FORCE3D_MASK") ? 1 : 0;
  return force3d_on() || g_force_mask;
}
static unsigned g_cur_fbo, g_cur_prog, g_cur_active_unit, g_cur_tex2d[8];
/* DIAG smear: por-FBO conta draws e clears -> FBO com draws>0 e clears==0 = a camada
   que nunca é limpa (causa do rastro). Dump periódico sob SONIC_CLEARLOG. */
static unsigned g_fbo_draws[32], g_fbo_clears[32];
static void fbo_stat_draw(void){ if(g_cur_fbo<32) g_fbo_draws[g_cur_fbo]++; }
static void fbo_stat_clear(void){ if(g_cur_fbo<32) g_fbo_clears[g_cur_fbo]++; }
static void fbo_stat_dump(void){
  fprintf(stderr,"[FBO-STATS] (draws/clears por FBO; draws>0 clears=0 = smear)\n");
  for(unsigned i=0;i<32;i++) if(g_fbo_draws[i]||g_fbo_clears[i])
    fprintf(stderr,"   FBO %u: draws=%u clears=%u%s\n",i,g_fbo_draws[i],g_fbo_clears[i],
            (g_fbo_draws[i]>50&&g_fbo_clears[i]==0)?"   <<< NUNCA LIMPO (SMEAR?)":"");
}
static int g_depth_test = -1, g_cull_face = -1, g_blend = -1, g_scissor = -1;
static int g_depth_mask = 1, g_color_mask[4] = {1, 1, 1, 1};
static unsigned g_depth_func, g_cull_mode, g_blend_src, g_blend_dst, g_blend_src_a, g_blend_dst_a;
static int g_viewport[4];

static int post_game_draw_window(void) {
  return sonic_frame_for_imports >= 1100;
}

static void force_visible_3d_state(int count) {
  if (!post_game_draw_window() || count < 16) return;
  if (!force3d_on() && !force_cull_on() && !force_depth_on() &&
      !force_scissor_on() && !force_mask_on()) return;
  static void (*p_color_mask)(unsigned char, unsigned char, unsigned char, unsigned char);
  static void (*p_depth_mask)(unsigned char);
  static void (*p_disable)(unsigned);
  static void (*p_blend_func)(unsigned, unsigned);
  if (!p_color_mask) p_color_mask = rgl("glColorMask");
  if (!p_depth_mask) p_depth_mask = rgl("glDepthMask");
  if (!p_disable) p_disable = rgl("glDisable");
  if (!p_blend_func) p_blend_func = rgl("glBlendFunc");
  if (force_mask_on() && p_color_mask) p_color_mask(1, 1, 1, 1);
  if (force_depth_on() && p_depth_mask) p_depth_mask(1);
  if (p_disable) {
    if (force_cull_on()) p_disable(0x0B44);    /* GL_CULL_FACE */
    if (force_depth_on()) p_disable(0x0B71);   /* GL_DEPTH_TEST */
    if (force_scissor_on()) p_disable(0x0C11); /* GL_SCISSOR_TEST */
  }
  if (p_blend_func) p_blend_func(0x0302, 0x0303); /* SRC_ALPHA, ONE_MINUS_SRC_ALPHA */
}

static void log_draw_call(const char *kind, unsigned mode, int first, int count,
                          unsigned type, const void *indices) {
  if (!drawlog_on() || !post_game_draw_window()) return;
  static unsigned long n = 0;
  n++;
  if (n <= 260 || (n % 500) == 0) {
    fprintf(stderr,
            "[DRAW f=%lu #%lu] %s mode=0x%x first=%d count=%d type=0x%x idx=%p "
            "fbo=%u prog=%u tex0=%u tex1=%u tex2=%u depth=%d dfunc=0x%x "
            "cull=%d cmode=0x%x blend=%d bfunc=0x%x/0x%x/0x%x/0x%x "
            "scissor=%d cmask=%d%d%d%d dmask=%d vp=%d,%d %dx%d\n",
            sonic_frame_for_imports, n, kind, mode, first, count, type, indices,
            g_cur_fbo, g_cur_prog, g_cur_tex2d[0], g_cur_tex2d[1], g_cur_tex2d[2],
            g_depth_test, g_depth_func, g_cull_face, g_cull_mode, g_blend,
            g_blend_src, g_blend_dst, g_blend_src_a, g_blend_dst_a, g_scissor,
            g_color_mask[0], g_color_mask[1], g_color_mask[2], g_color_mask[3],
            g_depth_mask, g_viewport[0], g_viewport[1], g_viewport[2], g_viewport[3]);
  }
}

static int g_fbo0clear = -1;
static void my_glBindFramebuffer(unsigned t, unsigned fb) {
  static void(*r)(unsigned,unsigned); if(!r)r=rgl("glBindFramebuffer");
  if (t == 0x8D40) g_cur_fbo = fb;
  if (gllog_on()) { static int z=0; if(z<60){fprintf(stderr,"[GL] BindFramebuffer(0x%x, fbo=%u)\n",t,fb);z++;} }
  r(t,fb);
  /* 🔧 SONIC_FBO0CLEAR: limpa o FBO 0 (tela final, que o engine NUNCA limpa -> smear)
     UMA vez por frame, no exato momento em que o engine o liga (antes de compor nele). */
  if (g_fbo0clear<0) g_fbo0clear = getenv("SONIC_FBO0CLEAR")?1:0;
  if (g_fbo0clear && t==0x8D40 && fb==0) {
    extern volatile unsigned long sonic_frame_for_imports;
    static unsigned long last=~0UL;
    if (sonic_frame_for_imports != last) { last = sonic_frame_for_imports;
      static void(*c)(unsigned); if(!c)c=rgl("glClear"); c(0x4100); }
  }
  /* 🧹 SONIC_CLEARALL: limpa CADA FBO (inclusive offscreen) UMA vez por frame, no 1º
     bind do frame. Testa a teoria do bug do cassino: o buffer dos feixes de luz é
     desenhado ADITIVO e não é limpo pro preto a cada frame no gameplay -> acumula ->
     estoura branco. Limpar no 1º bind/frame remove o acúmulo CROSS-FRAME preservando
     os passes DENTRO do mesmo frame (binds seguintes do mesmo fb não re-limpam).
     SONIC_CLEARALL_RGB=R,G,B opcional define a cor (default preto). */
  static int g_clearall = -1;
  if (g_clearall < 0)
    g_clearall = !sonic_env_on("SONIC_NO_CLEARALL") &&
                 sonic_env_on("SONIC_CLEARALL");
  if (g_clearall && t==0x8D40 && fb < 64) {
    extern volatile unsigned long sonic_frame_for_imports;
    static unsigned long fb_last[64]; static int init=0;
    if (!init){ for(int i=0;i<64;i++) fb_last[i]=~0UL; init=1; }
    if (fb_last[fb] != sonic_frame_for_imports) {
      fb_last[fb] = sonic_frame_for_imports;
      static void(*cc)(float,float,float,float); static void(*c)(unsigned);
      if(!cc)cc=rgl("glClearColor"); if(!c)c=rgl("glClear");
      float cr=0,cg=0,cb=0; const char*e=getenv("SONIC_CLEARALL_RGB");
      if(e) sscanf(e,"%f,%f,%f",&cr,&cg,&cb);
      /* preserva a cor de clear do engine: salva via glGetFloatv e restaura */
      static void(*gf)(unsigned,float*); if(!gf)gf=rgl("glGetFloatv");
      float save[4]={0,0,0,0}; if(gf) gf(0x0C22,save); /* GL_COLOR_CLEAR_VALUE */
#ifdef __aarch64__
      /* 🔑 v3: glClear RESPEITA scissor/colorMask/depthMask. Se o 1º bind do frame do
         FBO que acumula a luz acontece com scissor ligado ou máscara parcial (ordem de
         passes do v3 difere do v2), nosso clear vira no-op parcial -> o cassino segue
         acumulando (fundo estoura + duplicação). Forçar estado FULL p/ o clear e
         restaurar o estado rastreado do engine em seguida. */
      static void(*en)(unsigned); static void(*dis)(unsigned);
      static void(*cm)(unsigned char,unsigned char,unsigned char,unsigned char);
      static void(*dm)(unsigned char);
      if(!en)en=rgl("glEnable"); if(!dis)dis=rgl("glDisable");
      if(!cm)cm=rgl("glColorMask"); if(!dm)dm=rgl("glDepthMask");
      int had_sc = (g_scissor == 1);
      if (had_sc && dis) dis(0x0C11);          /* GL_SCISSOR_TEST off p/ o clear */
      if (cm) cm(1,1,1,1);
      if (dm) dm(1);
      cc(cr,cg,cb,1.0f); c(0x4100); /* COLOR|DEPTH */
      if (had_sc && en) en(0x0C11);
      if (cm) cm((unsigned char)g_color_mask[0],(unsigned char)g_color_mask[1],
                 (unsigned char)g_color_mask[2],(unsigned char)g_color_mask[3]);
      if (dm) dm((unsigned char)g_depth_mask);
#else
      cc(cr,cg,cb,1.0f); c(0x4100); /* COLOR|DEPTH */
#endif
      cc(save[0],save[1],save[2],save[3]);
    }
  }
}
static void my_glClearColor(float a,float b,float c,float d){
  static void(*r)(float,float,float,float); if(!r)r=rgl("glClearColor");
  if (gllog_on()) { static int z=0; if(z<30){fprintf(stderr,"[GL] ClearColor(%.2f,%.2f,%.2f,%.2f)\n",a,b,c,d);z++;} }
  r(a,b,c,d);
}
static void my_glClear(unsigned m){
  static void(*r)(unsigned); if(!r)r=rgl("glClear");
  /* SONIC_CLEARLOG: loga CADA glClear com o FBO ligado (g_cur_fbo) -> ver se o FBO da
     cena (RenderTarget != 0) é limpo por frame. Cap pequeno p/ capturar a estrutura. */
  fbo_stat_clear();
  if (getenv("SONIC_CLEARLOG")) { static unsigned long c=0;
      if(c<60) fprintf(stderr,"[CLEAR] fbo=%u mask=0x%x\n", g_cur_fbo, m);
      if((++c % 600)==0) fbo_stat_dump(); }
  else if (gllog_on()) { static unsigned long n=0; if((n++ % 120)==0)fprintf(stderr,"[GL] Clear mask=0x%x (#%lu)\n",m,n); }
  r(m);
}
static int g_glerr = -1;
static int glerr_on(void){ if(g_glerr<0) g_glerr=getenv("SONIC_GLERR")?1:0; return g_glerr; }
static unsigned (*p_glGetError)(void);
static unsigned (*p_glCheckFB)(unsigned);
static unsigned (*p_glGetIntegerv_fb)(void); /* placeholder */
static void glerr_check(const char*tag){
  if(!glerr_on()) return;
  if(!p_glGetError) p_glGetError=rgl("glGetError");
  if(!p_glCheckFB) p_glCheckFB=rgl("glCheckFramebufferStatus");
  unsigned e=p_glGetError?p_glGetError():0;
  if(e){ static unsigned long n=0; if(n++<40){
    unsigned st=p_glCheckFB?p_glCheckFB(0x8D40):0;
    fprintf(stderr,"[GLERR] %s err=0x%x fbStatus=0x%x\n",tag,e,st);} }
}
static void my_glDrawElements(unsigned md,int c,unsigned t,const void*i){
  static void(*r)(unsigned,int,unsigned,const void*); if(!r)r=rgl("glDrawElements");
  fbo_stat_draw();
  if (gllog_on()) { static unsigned long n=0; if((n++ % 500)==0)fprintf(stderr,"[GL] DrawElements #%lu count=%d\n",n,c); }
  force_visible_3d_state(c);
  log_draw_call("elements", md, -1, c, t, i);
  r(md,c,t,i);
  glerr_check("DrawElements");
}
static void my_glDrawArrays(unsigned md,int f,int c){
  static void(*r)(unsigned,int,int); if(!r)r=rgl("glDrawArrays");
  fbo_stat_draw();
  if (gllog_on()) { static unsigned long n=0; if((n++ % 500)==0)fprintf(stderr,"[GL] DrawArrays #%lu count=%d\n",n,c); }
  force_visible_3d_state(c);
  log_draw_call("arrays", md, f, c, 0, NULL);
  r(md,f,c);
  glerr_check("DrawArrays");
}
static unsigned my_glCheckFramebufferStatus(unsigned t){
  if(!p_glCheckFB) p_glCheckFB=rgl("glCheckFramebufferStatus");
  unsigned st=p_glCheckFB(t);
  if(glerr_on() && st!=0x8CD5){ static int z=0; if(z++<40)
    fprintf(stderr,"[GLERR] CheckFramebufferStatus(0x%x)=0x%x INCOMPLETO!\n",t,st); }
  return st;
}
static void my_glRenderbufferStorage(unsigned t,unsigned ifmt,int w,int h){
  static void(*r)(unsigned,unsigned,int,int); if(!r)r=rgl("glRenderbufferStorage");
  if(glerr_on()){static int z=0; if(z++<30)
    fprintf(stderr,"[FBO] RenderbufferStorage ifmt=0x%x %dx%d\n",ifmt,w,h);}
  r(t,ifmt,w,h);
}
static void my_glTexImage2D(unsigned t,int l,int ifmt,int w,int h,int b,unsigned fmt,unsigned ty,const void*p){
  static void(*r)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*);
  if(!r)r=rgl("glTexImage2D");
  if(glerr_on()){static int z=0; if(z++<80)
    fprintf(stderr,"%s TexImage2D ifmt=0x%x %dx%d fmt=0x%x type=0x%x data=%p\n",
            p ? "[TEX]" : "[FBO]", ifmt,w,h,fmt,ty,p);}
  r(t,l,ifmt,w,h,b,fmt,ty,p);
  glerr_check("TexImage2D");
}
static void my_glCompressedTexImage2D(unsigned t,int l,unsigned ifmt,int w,int h,
                                      int b,int imageSize,const void*p){
  static void(*r)(unsigned,int,unsigned,int,int,int,int,const void*);
  if(!r)r=rgl("glCompressedTexImage2D");
  if(glerr_on()){static int z=0; if(z++<120)
    fprintf(stderr,"[CTEX] CompressedTexImage2D ifmt=0x%x %dx%d size=%d data=%p\n",
            ifmt,w,h,imageSize,p);}
  r(t,l,ifmt,w,h,b,imageSize,p);
  glerr_check("CompressedTexImage2D");
}
static void my_glUseProgram(unsigned p){
  static void(*r)(unsigned); if(!r)r=rgl("glUseProgram");
  g_cur_prog = p;
  if(glerr_on()){static int z=0; if(z++<120)
    fprintf(stderr,"[GL] UseProgram(%u)\n",p);}
  r(p);
  glerr_check("UseProgram");
}
static void my_glBindTexture(unsigned t,unsigned tex){
  static void(*r)(unsigned,unsigned); if(!r)r=rgl("glBindTexture");
  if (t == 0x0DE1 && g_cur_active_unit < 8) g_cur_tex2d[g_cur_active_unit] = tex;
  if(glerr_on()){static int z=0; if(z++<160)
    fprintf(stderr,"[GL] BindTexture(target=0x%x, tex=%u)\n",t,tex);}
  r(t,tex);
}
static void my_glActiveTexture(unsigned tex){
  static void(*r)(unsigned); if(!r)r=rgl("glActiveTexture");
  if (tex >= 0x84C0 && tex < 0x84C8) g_cur_active_unit = tex - 0x84C0;
  r(tex);
}
static void my_glEnable(unsigned cap){
  static void(*r)(unsigned); if(!r)r=rgl("glEnable");
  if (cap == 0x0B71) g_depth_test = 1;
  else if (cap == 0x0B44) g_cull_face = 1;
  else if (cap == 0x0BE2) g_blend = 1;
  else if (cap == 0x0C11) g_scissor = 1;
  if (drawlog_on() && post_game_draw_window()) {
    static int z=0; if(z++<120) fprintf(stderr,"[STATE f=%lu] Enable 0x%x\n", sonic_frame_for_imports, cap);
  }
  r(cap);
}
static void my_glDisable(unsigned cap){
  static void(*r)(unsigned); if(!r)r=rgl("glDisable");
  if (cap == 0x0B71) g_depth_test = 0;
  else if (cap == 0x0B44) g_cull_face = 0;
  else if (cap == 0x0BE2) g_blend = 0;
  else if (cap == 0x0C11) g_scissor = 0;
  if (drawlog_on() && post_game_draw_window()) {
    static int z=0; if(z++<120) fprintf(stderr,"[STATE f=%lu] Disable 0x%x\n", sonic_frame_for_imports, cap);
  }
  r(cap);
}
static void my_glColorMask(unsigned char r0,unsigned char g,unsigned char b,unsigned char a){
  static void(*r)(unsigned char,unsigned char,unsigned char,unsigned char); if(!r)r=rgl("glColorMask");
  g_color_mask[0] = !!r0; g_color_mask[1] = !!g; g_color_mask[2] = !!b; g_color_mask[3] = !!a;
  if (drawlog_on() && post_game_draw_window()) {
    static int z=0; if(z++<80) fprintf(stderr,"[STATE f=%lu] ColorMask %d%d%d%d\n", sonic_frame_for_imports, g_color_mask[0], g_color_mask[1], g_color_mask[2], g_color_mask[3]);
  }
  r(r0,g,b,a);
}
static void my_glDepthMask(unsigned char v){
  static void(*r)(unsigned char); if(!r)r=rgl("glDepthMask");
  g_depth_mask = !!v;
  if (drawlog_on() && post_game_draw_window()) {
    static int z=0; if(z++<80) fprintf(stderr,"[STATE f=%lu] DepthMask %d\n", sonic_frame_for_imports, g_depth_mask);
  }
  r(v);
}
static void my_glDepthFunc(unsigned f){
  static void(*r)(unsigned); if(!r)r=rgl("glDepthFunc");
  g_depth_func = f;
  r(f);
}
static void my_glCullFace(unsigned m){
  static void(*r)(unsigned); if(!r)r=rgl("glCullFace");
  g_cull_mode = m;
  r(m);
}
static void my_glBlendFunc(unsigned s,unsigned d){
  static void(*r)(unsigned,unsigned); if(!r)r=rgl("glBlendFunc");
  g_blend_src = s; g_blend_dst = d; g_blend_src_a = s; g_blend_dst_a = d;
  r(s,d);
}
static void my_glBlendFuncSeparate(unsigned sr,unsigned dr,unsigned sa,unsigned da){
  static void(*r)(unsigned,unsigned,unsigned,unsigned); if(!r)r=rgl("glBlendFuncSeparate");
  g_blend_src = sr; g_blend_dst = dr; g_blend_src_a = sa; g_blend_dst_a = da;
  r(sr,dr,sa,da);
}
static void my_glViewport(int x,int y,int w,int h){
  static void(*r)(int,int,int,int); if(!r)r=rgl("glViewport");
  g_viewport[0]=x; g_viewport[1]=y; g_viewport[2]=w; g_viewport[3]=h;
  r(x,y,w,h);
}
static void my_glFramebufferTexture2D(unsigned t,unsigned att,unsigned tt,unsigned tex,int l){
  static void(*r)(unsigned,unsigned,unsigned,unsigned,int); if(!r)r=rgl("glFramebufferTexture2D");
  if(glerr_on()){static int z=0; if(z++<30)
    fprintf(stderr,"[FBO] FramebufferTexture2D att=0x%x textarget=0x%x tex=%u\n",att,tt,tex);}
  r(t,att,tt,tex,l);
}
static void my_glFramebufferRenderbuffer(unsigned t,unsigned att,unsigned rt,unsigned rb){
  static void(*r)(unsigned,unsigned,unsigned,unsigned); if(!r)r=rgl("glFramebufferRenderbuffer");
  if(glerr_on()){static int z=0; if(z++<30)
    fprintf(stderr,"[FBO] FramebufferRenderbuffer att=0x%x rb=%u\n",att,rb);}
  r(t,att,rt,rb);
}

/* __gnu_Unwind_Find_exidx: o unwinder ESTÁTICO do jogo chama isto p/ achar a
 * tabela .ARM.exidx do módulo que contém o PC. O módulo é custom-loaded (não
 * dlopen) -> o __gnu_Unwind_Find_exidx do glibc (via dl_iterate_phdr) não o
 * conhece -> throw não acha o catch -> std::terminate. Aqui retornamos o
 * .ARM.exidx do nosso módulo p/ PCs dentro dele, e delegamos o resto. */
extern volatile uintptr_t g_load_base;
#define SH_EXIDX_VADDR 0x2e1198u
#define SH_EXIDX_SIZE  0xc0b8u
#define SH_TEXT_SIZE   0x340c58u
static void *my_find_exidx(uintptr_t pc, int *pcount) {
  uintptr_t lb = g_load_base;
  if (getenv("SONIC_EXIDXLOG") && lb && pc >= lb && pc < lb + SH_TEXT_SIZE)
    fprintf(stderr, "[EXIDX] frame off=0x%lx\n", (unsigned long)(pc - lb));
  if (lb && pc >= lb && pc < lb + SH_TEXT_SIZE) {
    if (pcount) *pcount = (int)(SH_EXIDX_SIZE / 8);
    return (void *)(lb + SH_EXIDX_VADDR);
  }
  static void *(*real)(uintptr_t, int *) = NULL;
  static int tried = 0;
  if (!tried) { tried = 1; real = dlsym(RTLD_DEFAULT, "__gnu_Unwind_Find_exidx"); }
  if (real && real != (void *)my_find_exidx) return real(pc, pcount);
  if (pcount) *pcount = 0;
  return NULL;
}

/* egl_shim chama isto p/ permitir override de glGetProcAddress; sem override
 * Sonic usa GLES2 do device direto -> NULL = usa a função real. */
void *sonic_gl_proc_override(const char *name) { (void)name; return NULL; }

/* ---- bionic-compat extra: a libfox v3 (NDK r21d) importa fortify _chk + helpers
   bionic que a glibc não tem (ou tem com ABI diferente). Wrappers -> libc real. ---- */
static size_t b_strlen_chk(const char *s, size_t n) { (void)n; return strlen(s); }
static char  *b_strchr_chk(const char *s, int c, size_t n) { (void)n; return strchr(s, c); }
static char  *b_strrchr_chk(const char *s, int c, size_t n) { (void)n; return strrchr(s, c); }
static char  *b_strncpy_chk2(char *d, const char *s, size_t n, size_t dn) { (void)dn; return strncpy(d, s, n); }
static void   b_FD_SET_chk(int fd, fd_set *set, size_t n) { (void)n; if (fd >= 0) FD_SET(fd, set); }
static void   b_FD_CLR_chk(int fd, fd_set *set, size_t n) { (void)n; if (fd >= 0) FD_CLR(fd, set); }
static int    b_FD_ISSET_chk(int fd, fd_set *set, size_t n) { (void)n; return (fd >= 0) ? FD_ISSET(fd, set) : 0; }
static void   b_android_set_abort_message(const char *m) { if (m) fprintf(stderr, "[android_abort] %s\n", m); }
static int b_sigsetjmp(sigjmp_buf e, int save) { return sigsetjmp(e, save); }
/* funopen: a libfox v3 importa. Resolvido em RUNTIME via dlsym (a glibc do device
   tem; evita dependência de link, que falha em alguns sysroots). */
static FILE *b_funopen(const void *c, void *r, void *w, void *s, void *cl) {
  static FILE *(*real)(const void *, void *, void *, void *, void *) = NULL;
  if (!real) real = (FILE *(*)(const void *, void *, void *, void *, void *))
                    dlsym(RTLD_DEFAULT, "funopen");
  return real ? real(c, r, w, s, cl) : NULL;
}

/* ---------------- tabela de overrides ---------------- */
DynLibFunction shantae_overrides[] = {
    /* bionic-compat extra (libfox v3 fortify/_chk + helpers) */
    {"__strlen_chk", (uintptr_t)b_strlen_chk},
    {"__strchr_chk", (uintptr_t)b_strchr_chk},
    {"__strrchr_chk", (uintptr_t)b_strrchr_chk},
    {"__strncpy_chk2", (uintptr_t)b_strncpy_chk2},
    {"__FD_SET_chk", (uintptr_t)b_FD_SET_chk},
    {"__FD_CLR_chk", (uintptr_t)b_FD_CLR_chk},
    {"__FD_ISSET_chk", (uintptr_t)b_FD_ISSET_chk},
    {"android_set_abort_message", (uintptr_t)b_android_set_abort_message},
    {"funopen", (uintptr_t)b_funopen},
    {"sigsetjmp", (uintptr_t)b_sigsetjmp},
    /* liblog */
    {"__android_log_print", (uintptr_t)b_log_print},
    {"__android_log_write", (uintptr_t)b_log_write},
    {"__android_log_assert", (uintptr_t)b_log_assert},
    {"__android_log_vprint", (uintptr_t)b_log_vprint},
    /* bionic-only libc */
    {"strlcpy", (uintptr_t)b_strlcpy},
    {"strlcat", (uintptr_t)b_strlcat},
    {"__assert2", (uintptr_t)b_assert2},
    {"__system_property_get", (uintptr_t)b_system_property_get},
    {"stat", (uintptr_t)my_stat},
    {"lstat", (uintptr_t)my_lstat},
    {"fstat", (uintptr_t)my_fstat},
    {"__errno", (uintptr_t)b_errno},
    {"__stack_chk_fail", (uintptr_t)my_stack_chk_fail},
    {"__gnu_Unwind_Find_exidx", (uintptr_t)my_find_exidx},
    /* pthread_attr_* e pthread_create -> revc_pthread_table (bridge) */
    /* stdio bridge bionic->glibc (stdio_shim.c) — __sF/_ctype_/_toupper_tab_
     * resolvem por dlsym (símbolos exportados); funções abaixo precisam OVERRIDE
     * (a glibc nativa crasharia com FILE* do array __sF). */
    {"fwrite", (uintptr_t)b_fwrite},
    {"fopen", (uintptr_t)b_fopen},
    {"fread", (uintptr_t)b_fread},
    {"fputs", (uintptr_t)b_fputs},
    {"fputc", (uintptr_t)b_fputc},
    {"fgetc", (uintptr_t)b_fgetc},
    {"fseek", (uintptr_t)b_fseek},
    {"ftell", (uintptr_t)b_ftell},
    {"fflush", (uintptr_t)b_fflush},
    {"fclose", (uintptr_t)b_fclose},
    {"memcpy", (uintptr_t)my_memcpy},
    {"memmove", (uintptr_t)my_memmove},
    {"__memcpy_chk", (uintptr_t)my_memcpy_chk},
    {"__aeabi_memcpy", (uintptr_t)my_aeabi_memcpy},
    {"__aeabi_memcpy4", (uintptr_t)my_aeabi_memcpy},
    {"__aeabi_memcpy8", (uintptr_t)my_aeabi_memcpy},
    {"__aeabi_memmove", (uintptr_t)my_aeabi_memmove},
    {"__aeabi_memmove4", (uintptr_t)my_aeabi_memmove},
    {"__aeabi_memmove8", (uintptr_t)my_aeabi_memmove},
    {"__aeabi_memset", (uintptr_t)my_aeabi_memset},
    {"__aeabi_memset4", (uintptr_t)my_aeabi_memset},
    {"__aeabi_memset8", (uintptr_t)my_aeabi_memset},
    /* EGL -> egl_shim (GLES2 via SDL2, Mali fbdev) */
    {"eglGetDisplay", (uintptr_t)egl_shim_GetDisplay},
    {"eglInitialize", (uintptr_t)egl_shim_Initialize},
    {"eglTerminate", (uintptr_t)egl_shim_Terminate},
    {"eglChooseConfig", (uintptr_t)egl_shim_ChooseConfig},
    {"eglCreateWindowSurface", (uintptr_t)egl_shim_CreateWindowSurface},
    {"eglCreatePbufferSurface", (uintptr_t)egl_shim_CreatePbufferSurface},
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
    {"eglQuerySurface", (uintptr_t)egl_shim_QuerySurface},
    {"eglQueryString", (uintptr_t)egl_shim_QueryString},
    {"eglGetCurrentContext", (uintptr_t)egl_shim_GetCurrentContext},
    {"eglGetCurrentSurface", (uintptr_t)egl_shim_GetCurrentSurface},
    {"eglSurfaceAttrib", (uintptr_t)egl_shim_SurfaceAttrib},
    {"eglReleaseThread", (uintptr_t)egl_releasethread_stub},
    /* ANativeWindow */
    {"ANativeWindow_fromSurface", (uintptr_t)aw_fromSurface},
    {"ANativeWindow_acquire", (uintptr_t)aw_acquire},
    {"ANativeWindow_release", (uintptr_t)aw_release},
    {"ANativeWindow_getWidth", (uintptr_t)aw_getWidth},
    {"ANativeWindow_getHeight", (uintptr_t)aw_getHeight},
    {"ANativeWindow_getFormat", (uintptr_t)aw_getFormat},
    {"ANativeWindow_setBuffersGeometry", (uintptr_t)aw_setBuffersGeometry},
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
    {"AMotionEvent_getButtonState", (uintptr_t)ame_getButtonState},
    /* OpenSL ES */
    /* GL logging (diag tela preta). glClearColor NÃO aqui -> cai no
     * softfp_resolve (sf_glClearColor) por causa do ABI float softfp/hardfp. */
    {"glBindFramebuffer", (uintptr_t)my_glBindFramebuffer},
    {"glClear", (uintptr_t)my_glClear},
    {"glDrawElements", (uintptr_t)my_glDrawElements},
    {"glDrawArrays", (uintptr_t)my_glDrawArrays},
    {"glCheckFramebufferStatus", (uintptr_t)my_glCheckFramebufferStatus},
    {"glRenderbufferStorage", (uintptr_t)my_glRenderbufferStorage},
    {"glTexImage2D", (uintptr_t)my_glTexImage2D},
    {"glCompressedTexImage2D", (uintptr_t)my_glCompressedTexImage2D},
    {"glUseProgram", (uintptr_t)my_glUseProgram},
    {"glBindTexture", (uintptr_t)my_glBindTexture},
    {"glActiveTexture", (uintptr_t)my_glActiveTexture},
    {"glEnable", (uintptr_t)my_glEnable},
    {"glDisable", (uintptr_t)my_glDisable},
    {"glColorMask", (uintptr_t)my_glColorMask},
    {"glDepthMask", (uintptr_t)my_glDepthMask},
    {"glDepthFunc", (uintptr_t)my_glDepthFunc},
    {"glCullFace", (uintptr_t)my_glCullFace},
    {"glBlendFunc", (uintptr_t)my_glBlendFunc},
    {"glBlendFuncSeparate", (uintptr_t)my_glBlendFuncSeparate},
    {"glViewport", (uintptr_t)my_glViewport},
    {"glFramebufferTexture2D", (uintptr_t)my_glFramebufferTexture2D},
    {"glFramebufferRenderbuffer", (uintptr_t)my_glFramebufferRenderbuffer},
    {"slCreateEngine", (uintptr_t)slCreateEngine_shim},
    {"SL_IID_ENGINE", (uintptr_t)&SL_IID_ENGINE_v},
    {"SL_IID_PLAY", (uintptr_t)&SL_IID_PLAY_v},
    {"SL_IID_RECORD", (uintptr_t)&SL_IID_RECORD_v},
    {"SL_IID_VOLUME", (uintptr_t)&SL_IID_VOLUME_v},
    {"SL_IID_BUFFERQUEUE", (uintptr_t)&SL_IID_BUFFERQUEUE_v},
    {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&SL_IID_ANDROIDSIMPLEBUFFERQUEUE_v},
    {"SL_IID_ANDROIDCONFIGURATION", (uintptr_t)&SL_IID_ANDROIDCONFIGURATION_v},
};
const int shantae_overrides_count =
    sizeof(shantae_overrides) / sizeof(shantae_overrides[0]);
