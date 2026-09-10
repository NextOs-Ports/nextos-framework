/*
 * imports.c -- superficie de import do loader de Titan Souls (ARM32 softfp).
 *
 * A tabela VENCE o fallback dlsym do so_resolve. Entra aqui tudo o que tem
 * ABI ou layout diferente do host:
 *
 *   - fronteira SOFTFP: a engine e o FMOD nao tem Tag_ABI_VFP_args, entao
 *     todo simbolo que passa/devolve float ou double precisa de wrapper com
 *     pcs("aapcs"). Errar aqui NAO crasha: o valor vira lixo em silencio
 *     (audio surdo, fisica maluca) — e' o risco #1 deste port.
 *   - FILE/stat/dirent/pthread/sem: bionic ARM32 e' MENOR que glibc.
 *   - /sdcard cravado no binario -> traducao de caminho.
 *   - EGL -> egl_shim (contexto GLES2 via SDL2, caminho Mali).
 *   - asset/looper/input/window -> shims do loader.
 *   - OpenSL ES -> opensles_shim (o FMOD Ex sai por aqui).
 *
 * O resto (memcpy, strlen, glDrawArrays...) cai no dlsym e vai direto para a
 * glibc/Mali do device: mesma ABI, sem custo.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <fcntl.h>
#include <regex.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "android_shim.h"
#include "asset_shim.h"
#include "bionic_compat.h"
#include "cpuinfo_compat.h"
#include "egl_shim.h"
#include "hueshift_shader_fix.h"
#include "imports.h"
#include "opensles_shim.h"
#include "platform_shims.h"
#include "pthread_bridge.h"
#include "softfp_bridge.h"
#include "tilemap_uv_fix.h"
#include "util.h"

#define TS(name, target) { name, (uintptr_t)(target) }

extern int asm2_setjmp(void *buffer);
extern void asm2_longjmp(void *buffer, int value) __attribute__((noreturn));

/* ---- libgcc AEABI: os helpers do AEABI usam SEMPRE a PCS base (registrador
 * inteiro), independente do float-abi de quem chama. Referenciamos os simbolos
 * do libgcc do host explicitamente porque, estatico, eles so' entram no binario
 * se alguem os citar — e sem eles a engine fica com GOT vazio. ---- */
extern void __aeabi_idiv(void);
extern void __aeabi_idivmod(void);
extern void __aeabi_uidiv(void);
extern void __aeabi_uidivmod(void);
extern void __aeabi_uldivmod(void);
extern void __aeabi_ldivmod(void);
extern void __aeabi_d2lz(void);
extern void __aeabi_d2ulz(void);
extern void __aeabi_ul2d(void);
extern void __aeabi_l2d(void);
extern void __aeabi_unwind_cpp_pr0(void);
extern void __aeabi_unwind_cpp_pr1(void);
extern void __aeabi_unwind_cpp_pr2(void);

/* ---- C++ runtime minimo -------------------------------------------------
 * O libfmodex.so e' carregado ANTES da engine (ele e' autocontido e ela
 * cross-resolve nele), mas o `operator delete` que ele usa mora na engine.
 * Em vez de inverter a ordem de carga — o que quebraria o cross-resolve —
 * damos a versao do loader: o `new` daquela era e' malloc puro, entao free()
 * casa. */
static void *ts_op_new(size_t n) {
  void *p = malloc(n ? n : 1);
  if (!p) {
    logPrintf("operator new(%u) FALHOU\n", (unsigned)n);
    abort();
  }
  return p;
}
static void ts_op_delete(void *p) { free(p); }
static void ts_pure_virtual(void) {
  logPrintf("__cxa_pure_virtual chamado (objeto meio-construido)\n");
  abort();
}

/* ---- regex: bionic regex_t (FreeBSD, 16 B em ARM32) e' MENOR que o da
 * glibc (~32 B). A engine reserva o tamanho bionic na pilha dela; deixar a
 * glibc escrever ali e' smash na certa. Guardamos um regex_t de verdade no
 * heap e so' o ponteiro no objeto do convidado. ---- */
struct ts_bionic_regex { void *host; unsigned nsub; void *pad[2]; };

static int ts_regcomp(void *guest, const char *pattern, int flags) {
  struct ts_bionic_regex *g = (struct ts_bionic_regex *)guest;
  regex_t *h = (regex_t *)calloc(1, sizeof(regex_t));
  if (!h) return REG_ESPACE;
  int r = regcomp(h, pattern, flags);
  if (r != 0) { free(h); return r; }
  g->host = h;
  g->nsub = (unsigned)h->re_nsub;
  return 0;
}
static int ts_regexec(const void *guest, const char *text, size_t nmatch,
                      regmatch_t *pmatch, int flags) {
  const struct ts_bionic_regex *g = (const struct ts_bionic_regex *)guest;
  if (!g || !g->host) return REG_NOMATCH;
  return regexec((const regex_t *)g->host, text, nmatch, pmatch, flags);
}
static void ts_regfree(void *guest) {
  struct ts_bionic_regex *g = (struct ts_bionic_regex *)guest;
  if (!g || !g->host) return;
  regfree((regex_t *)g->host);
  free(g->host);
  g->host = NULL;
}

/* ---- A engine instala handler de sinal proprio (relatorio de crash da
 * casa). Se ela pegar SIGSEGV, o NOSSO diagnostico some e o processo morre
 * mudo. Logamos quem pediu o que, e deixamos passar — o fluxo e' dela. ---- */
static int ts_sigaction(int sig, const void *act, void *old) {
  if (act)
    logPrintf("engine: sigaction(%d) instalado pelo jogo\n", sig);
  return asm2_sigaction(sig, act, old);
}

/* ---- sysconf: o bridge do asm2 respondia so' PAGE_SIZE e devolvia
 * -1/ENOSYS no resto. Sem saber quantos nucleos existem, a engine cria o
 * AgPriorityThreadPool do audio SEM worker nenhum, e o job que le o stream de
 * musica (AudioStreamJob, empilhado pelo callback assincrono do FMOD) so'
 * roda quando a thread principal passa por AgAudioManagerFMOD::platformUpdate.
 * So' que a musica e' aberta com FMOD_NONBLOCKING e a engine espera por ela
 * ANTES disso, num laco proprio (AgAudioChannelFMOD::_play) — o job nunca roda
 * e o jogo fica na tela branca. Responder o numero de nucleos poe workers no
 * pool e o stream abre sozinho, que e' o comportamento do Android.
 * TS_CPUS ajusta sem rebuild. ---- */
static long ts_sysconf(int android_name) {
  switch (android_name) {
  case 0x60: /* _SC_NPROCESSORS_CONF */
  case 0x61: /* _SC_NPROCESSORS_ONLN */
  {
    const char *e = getenv("TS_CPUS");
    long host = sysconf(_SC_NPROCESSORS_ONLN);
    if (e && atoi(e) > 0) return atoi(e);
    return host > 0 ? host : 1;
  }
  case 0x62: return sysconf(_SC_PHYS_PAGES);
  case 0x63: return sysconf(_SC_AVPHYS_PAGES);
  default: return asm2_sysconf(android_name);
  }
}

/* ---- CPU capabilities across the 32/64-bit kernel boundary -------------
 * FMOD Ex 4.44 parses /proc/cpuinfo itself and only recognizes the ARMv7
 * spellings vfp/vfpv3/neon.  A 32-bit guest on an AArch64 kernel sees the
 * equivalent features as fp/asimd, so FMOD returns NEEDSHARDWARE before it
 * resolves a single OpenSL symbol.  Preserve the native probe and add only
 * proven aliases to the first read of this exact procfs file. */
static int g_cpuinfo_fd = -1;
static int g_cpuinfo_alias_log_emitted;

static int __attribute__((pcs("aapcs"))) ts_open(const char *path, int flags,
                                                   ...) {
  mode_t mode = 0;
  int fd;

  if (flags & O_CREAT) {
    va_list args;
    va_start(args, flags);
    mode = (mode_t)va_arg(args, int);
    va_end(args);
    fd = asm2_open(path, flags, mode);
  } else {
    fd = asm2_open(path, flags);
  }
  if (fd >= 0 && path && strcmp(path, "/proc/cpuinfo") == 0)
    g_cpuinfo_fd = fd;
  return fd;
}

static ssize_t __attribute__((pcs("aapcs"))) ts_read(int fd, void *buffer,
                                                       size_t count) {
  ssize_t result = read(fd, buffer, count);

  if (fd == g_cpuinfo_fd && result >= 0) {
    g_cpuinfo_fd = -1;
    if (result > 0) {
      size_t length = (size_t)result;
      if (ts_cpuinfo_add_armv7_aliases((char *)buffer, &length, count) == 1) {
        result = (ssize_t)length;
        if (!g_cpuinfo_alias_log_emitted) {
          logPrintf("[fmod/cpuinfo] fp/asimd -> vfp/vfpv3/neon para guest "
                    "ARMv7\n");
          g_cpuinfo_alias_log_emitted = 1;
        }
      }
    }
  }
  return result;
}

/* ---- pthread_setname_np recebe o pthread_t do BIONIC (um inteiro de 32
 * bits), nao o opaco da glibc. Passar isso direto para o host e' ponteiro
 * invalido. O nome da thread nao muda nada no jogo, entao a resposta honesta
 * e' aceitar e ignorar. ---- */
static int ts_pthread_setname_np(uint32_t guest_thread, const char *name) {
  (void)guest_thread; (void)name;
  return 0;
}

/* ---- OpenSL ES: o FMOD Ex NAO linka libOpenSLES — ele faz
 * dlopen("libOpenSLES.so") + dlsym em runtime. No device essa lib nao existe,
 * o dlopen do host devolve NULL e o FMOD fica sem saida de audio (foi
 * exatamente o que travou o carregamento da primeira faixa de musica).
 * Interpomos: dlopen devolve um handle magico e o dlsym dele responde com o
 * opensles_shim (OpenSL -> SDL2 -> ALSA do CFW). ---- */
void *ts_gl_proc_override(const char *name);

#define TS_SL_MAGIC ((void *)0x5151ABCDul)

static void *ts_sl_dlsym(const char *name) {
  if (!name) return NULL;
  { static int n; if (n++ < 24) logPrintf("[sl] dlsym %s\n", name); }
  if (strcmp(name, "slCreateEngine") == 0) return (void *)slCreateEngine_shim;
  if (strcmp(name, "SL_IID_ENGINE") == 0) return (void *)&sl_IID_ENGINE;
  if (strcmp(name, "SL_IID_PLAY") == 0) return (void *)&sl_IID_PLAY;
  if (strcmp(name, "SL_IID_VOLUME") == 0) return (void *)&sl_IID_VOLUME;
  if (strcmp(name, "SL_IID_BUFFERQUEUE") == 0) return (void *)&sl_IID_BUFFERQUEUE;
  if (strcmp(name, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE") == 0)
    return (void *)&sl_IID_BUFFERQUEUE;
  /* Interfaces que o shim nao implementa PRECISAM existir como simbolo: o
   * FMOD desiste da saida OpenSL inteira se um dos SL_IID_* que ele procura
   * vier NULL (medido: sem SL_IID_ANDROIDCONFIGURATION/RECORD ele nem abre o
   * device e o jogo fica MUDO). Devolvemos um IID proprio; o GetInterface do
   * shim responde com um stub inofensivo para qualquer iid desconhecido. */
  if (strncmp(name, "SL_IID_", 7) == 0) {
    static const void *tag;
    static const void *iid = &tag;
    return (void *)&iid;
  }
  if (strcmp(name, "SL_IID_EFFECTSEND") == 0) return (void *)&sl_IID_EFFECTSEND;
  if (strcmp(name, "SL_IID_ENGINECAPABILITIES") == 0)
    return (void *)&sl_IID_ENGINECAPABILITIES;
  if (strcmp(name, "SL_IID_ENVIRONMENTALREVERB") == 0)
    return (void *)&sl_IID_ENVIRONMENTALREVERB;
  logPrintf("[sl] dlsym %s -> NULL\n", name);
  return NULL;
}

static void *ts_dlopen(const char *name, int flag) {
  if (name && strstr(name, "OpenSLES")) {
    /* TS_NOAUDIO=1: dlopen falha e o FMOD cai em "nosound" e SEGUE o fluxo —
     * usado so' para separar problema de audio de problema de video. */
    if (getenv("TS_NOAUDIO")) {
      logPrintf("[sl] dlopen %s -> NULL (TS_NOAUDIO)\n", name);
      return NULL;
    }
    logPrintf("[sl] dlopen %s -> shim OpenSL\n", name);
    return TS_SL_MAGIC;
  }
  return dlopen(name, flag);
}

static void *ts_dlsym(void *handle, const char *name) {
  if (handle == TS_SL_MAGIC) return ts_sl_dlsym(name);
  { void *ov = ts_gl_proc_override(name); if (ov) return ov; }
  return dlsym(handle, name);
}

static int ts_dlclose(void *h) {
  if (h == TS_SL_MAGIC) return 0;
  return dlclose(h);
}

extern int ts_FMOD_System_Create(void **system);
extern int ts_fmod_createStream(void *system, const char *name,
                                unsigned int mode, void *exinfo, void **sound);
extern int ts_fmod_createSound(void *system, const char *name,
                               unsigned int mode, void *exinfo, void **sound);
extern int ts_fmod_setFileSystem(void *system, void *open, void *close,
                                 void *read, void *seek, void *aread,
                                 void *acancel, int align);
extern int ts_fmod_init(void *system, int maxchannels, unsigned int flags,
                        void *extradriverdata);

#define TS_SYM_STREAM \
  "_ZN4FMOD6System12createStreamEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE"
#define TS_SYM_SOUND \
  "_ZN4FMOD6System11createSoundEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE"
#define TS_SYM_SETFS                                                          \
  "_ZN4FMOD6System13setFileSystemEPF11FMOD_RESULTPKciPjPPvS6_EPFS1_S5_S5_EPF" \
  "S1_S5_S5_jS4_S5_EPFS1_S5_jS5_EPFS1_P18FMOD_ASYNCREADINFOS5_ESA_i"
#define TS_SYM_INIT "_ZN4FMOD6System4initEijPv"

/* ---- Amazon GameCircle: a lib nao e' carregada (periferico puro). Os
 * simbolos precisam existir para a engine linkar; devolvem "ok/desconectado"
 * e ENTREGAM o retorno, para o fluxo seguir (SDK que inicia e nunca responde
 * e' causa classica de travar no boot). ---- */
static void ag_noop(void) {}
static void *ag_null(void) { return NULL; }

/* ---- diagnostico de video: a primeira pergunta de "tela preta" e' se o
 * contexto veio mesmo do Mali. Uma linha, uma vez.
 *
 * Chama o glGetString REAL do driver, nunca o egl_shim_GetProcAddress: o
 * GetProcAddress consulta ts_gl_proc_override, que devolveria ESTA funcao — e
 * a recursao estoura a pilha da thread de textura. Foi exatamente o SIGSEGV
 * "dentro da libc" da primeira rodada. ---- */
extern const unsigned char *glGetString(unsigned name);
static const unsigned char *ts_glGetString(unsigned name) {
  const unsigned char *s = glGetString(name);
  static int logged = 0;
  if (!logged && name == 0x1F02 /* GL_VERSION */) {
    logged = 1;
    logPrintf("GL_VERSION='%s'\n", s ? (const char *)s : "(null)");
  }
  return s;
}

/* O patch de UV precisa atingir o mesmo varying nos dois estagios do programa.
 * Guardar o tipo devolvido por glCreateShader evita inferencia fragil a partir
 * do texto e faz o matcher forte de hueshift trabalhar no estagio correto. */
#define TS_GL_VERTEX_SHADER 0x8B31u
#define TS_GL_FRAGMENT_SHADER 0x8B30u

struct ts_shader_type_entry {
  unsigned id;
  unsigned type;
};
static struct ts_shader_type_entry g_shader_types[256];
static int g_shader_type_count;

static unsigned ts_glCreateShader(unsigned type) {
  unsigned id = glCreateShader(type);
  int i;

  if (!id || (type != TS_GL_VERTEX_SHADER && type != TS_GL_FRAGMENT_SHADER))
    return id;
  for (i = 0; i < g_shader_type_count; i++) {
    if (g_shader_types[i].id == id) {
      /* O driver pode reciclar um nome depois de glDeleteShader. */
      g_shader_types[i].type = type;
      return id;
    }
  }
  if (g_shader_type_count < (int)(sizeof(g_shader_types) /
                                  sizeof(g_shader_types[0]))) {
    g_shader_types[g_shader_type_count].id = id;
    g_shader_types[g_shader_type_count].type = type;
    g_shader_type_count++;
  }
  return id;
}

static unsigned ts_shader_type(unsigned id) {
  int i;
  for (i = 0; i < g_shader_type_count; i++)
    if (g_shader_types[i].id == id) return g_shader_types[i].type;
  return 0;
}

/* Dump das matrizes: uniform_Transform e uniform_Projection sao o que leva o
 * quad local 0..16 ate' o pixel. Se a translacao tiver parte fracionaria, a
 * borda do tile cai entre pixels e sobra o vao. TS_MDUMP=1. */
static void ts_glUniformMatrix4fv(int loc, int count, unsigned char transpose,
                                  const float *v) {
  static int n = -1;
  if (n < 0) n = getenv("TS_MDUMP") ? 0 : 999;
  if (n < 6 && v) {
    logPrintf("[mat] loc=%d count=%d\n", loc, count);
    logPrintf("[mat]  %.7f %.7f %.7f %.7f\n", (double)v[0],(double)v[1],(double)v[2],(double)v[3]);
    logPrintf("[mat]  %.7f %.7f %.7f %.7f\n", (double)v[4],(double)v[5],(double)v[6],(double)v[7]);
    logPrintf("[mat]  %.7f %.7f %.7f %.7f\n", (double)v[8],(double)v[9],(double)v[10],(double)v[11]);
    logPrintf("[mat]  %.7f %.7f %.7f %.7f\n", (double)v[12],(double)v[13],(double)v[14],(double)v[15]);
    n++;
  }
  glUniformMatrix4fv(loc, count, transpose, v);
}

/* Valor REAL que chega nos uniforms escalares/vetoriais do shader do tilemap.
 * `scale` decide o snap `floor(pos*scale)/scale`; se chegar errado por um
 * triz (fronteira SOFTFP!), o floor derruba o tile 1 px. TS_UDUMP=1. */
static int ts_glGetUniformLocation(unsigned prog, const char *name) {
  int loc = glGetUniformLocation(prog, name);
  static int n = -1;
  if (n < 0) n = getenv("TS_UDUMP") ? 0 : 999;
  if (n < 40) { logPrintf("[uniloc] prog=%u \"%s\" -> %d\n", prog, name ? name : "?", loc); n++; }
  return loc;
}
static void ts_glUniform1fv(int loc, int count, const float *v) {
  static int n = -1;
  if (n < 0) n = getenv("TS_UDUMP") ? 0 : 999;
  if (n < 16 && v) { logPrintf("[uni] 1fv loc=%d count=%d = %.9f (bits %08x)\n",
                               loc, count, (double)v[0], *(const unsigned *)v); n++; }
  glUniform1fv(loc, count, v);
}
void ts_uni1f_probe(int loc, float v) {
  static int n = -1;
  if (n < 0) n = getenv("TS_UDUMP") ? 0 : 999;
  if (n < 12) { logPrintf("[uni] glUniform1f loc=%d = %.9f (bits %08x)\n",
                          loc, (double)v, *(unsigned *)&v); n++; }
  glUniform1f(loc, v);
}
static void ts_glUniform2fv(int loc, int count, const float *v) {
  static int n = -1;
  if (n < 0) n = getenv("TS_UDUMP") ? 0 : 999;
  if (n < 12 && v) { logPrintf("[uni] glUniform2fv loc=%d = (%.9f, %.9f)\n",
                               loc, (double)v[0], (double)v[1]); n++; }
  glUniform2fv(loc, count, v);
}

/* Log de compilacao/link: sem isso, shader que falha some CALADO e a gente
 * so' ve' "personagem invisivel" — foi o que atrapalhou duas rodadas. */
static void ts_glCompileShader(unsigned shader) {
  glCompileShader(shader);
  int ok = 0;
  glGetShaderiv(shader, 0x8B81 /* GL_COMPILE_STATUS */, &ok);
  if (!ok) {
    char log[1024]; int n = 0;
    glGetShaderInfoLog(shader, (int)sizeof(log) - 1, &n, log);
    log[n > 0 && n < (int)sizeof(log) ? n : 0] = 0;
    logPrintf("[shader] COMPILE FALHOU id=%u: %s\n", shader, log);
  }
}

static void ts_glLinkProgram(unsigned prog) {
  glLinkProgram(prog);
  int ok = 0;
  glGetProgramiv(prog, 0x8B82 /* GL_LINK_STATUS */, &ok);
  if (!ok) {
    char log[1024]; int n = 0;
    glGetProgramInfoLog(prog, (int)sizeof(log) - 1, &n, log);
    log[n > 0 && n < (int)sizeof(log) ? n : 0] = 0;
    logPrintf("[shader] LINK FALHOU prog=%u: %s\n", prog, log);
  }
}

static void ts_glShaderSource(unsigned shader, int count,
                              const char *const *string, const int *length) {
  enum ts_hueshift_shader_stage stage;
  static unsigned logged_stages;
  unsigned type = ts_shader_type(shader);

  if (type == TS_GL_VERTEX_SHADER)
    stage = TS_HUESHIFT_SHADER_VERTEX;
  else if (type == TS_GL_FRAGMENT_SHADER)
    stage = TS_HUESHIFT_SHADER_FRAGMENT;
  else {
    glShaderSource(shader, count, string, length);
    return;
  }
  if (count <= 0) {
    glShaderSource(shader, count, string, length);
    return;
  }
  size_t total = 0;
  for (int i = 0; i < count; i++)
    total += (length && length[i] >= 0) ? (size_t)length[i]
                                        : (string[i] ? strlen(string[i]) : 0);
  char *joined = (char *)malloc(total + 1);
  if (!joined) { glShaderSource(shader, count, string, length); return; }
  size_t o = 0;
  for (int i = 0; i < count; i++) {
    size_t n = (length && length[i] >= 0) ? (size_t)length[i]
                                          : (string[i] ? strlen(string[i]) : 0);
    if (n && string[i]) { memcpy(joined + o, string[i], n); o += n; }
  }
  joined[o] = 0;
  char *fixed = ts_hueshift_shader_promote_uv(joined, stage);

  if (fixed) {
    unsigned stage_bit = 1u << (unsigned)stage;
    const char *one = fixed;
    if (!(logged_stages & stage_bit)) {
      logPrintf("[tilemap] hueshift %s: v_texCoord lowp -> mediump\n",
                stage == TS_HUESHIFT_SHADER_VERTEX ? "VS" : "FS");
      logged_stages |= stage_bit;
    }
    glShaderSource(shader, 1, &one, NULL);
    free(fixed);
  } else glShaderSource(shader, count, string, length);
  free(joined);
}

static int g_vstride = 0, g_vpos_off = 0;
static void ts_glVertexAttribPointer(unsigned idx, int size, unsigned type,
                                     unsigned char norm, int stride,
                                     const void *ptr) {
  static int logged = 0;
  if (getenv("TS_VDUMP") && logged < 6) {
    logPrintf("[vattr] idx=%u size=%d type=0x%x stride=%d off=%ld\n",
              idx, size, type, stride, (long)(uintptr_t)ptr);
    logged++;
  }
  if (idx == 0) { g_vstride = stride; g_vpos_off = (int)(uintptr_t)ptr; }
  glVertexAttribPointer(idx, size, type, norm, stride, ptr);
}

static void ts_glBufferData(unsigned target, long size, const void *data,
                            unsigned usage) {
  static int dumps = -1;
  static int uv_fix_logged;
  if (dumps < 0) dumps = getenv("TS_VDUMP") ? 0 : 99;
  /* so' buffers GRANDES: o tilemap sobe muitos quads de uma vez; o quad de
   * tela cheia do titulo tem 288 bytes e nao interessa. */
  if (dumps < 3 && data && size >= 4000) {
    int st = g_vstride > 0 ? g_vstride : 16;
    int nf = st / 4;
    const float *f = (const float *)data;
    logPrintf("[vdump] size=%ld stride=%d floats/vert=%d verts=%ld\n",
              size, st, nf, size / st);
    for (int v = 0; v < 8; v++) {
      char line[512]; int o2 = 0;
      o2 += snprintf(line + o2, sizeof(line) - o2, "[vdump] v%02d:", v);
      for (int k = 0; k < nf && o2 < 460; k++)
        o2 += snprintf(line + o2, sizeof(line) - o2, " %.4f", (double)f[v * nf + k]);
      logPrintf("%s\n", line);
    }
    dumps++;
  }

  /* HueTilemap usa um atlas 1024x1024, quads fonte 16x16 e GL_NEAREST. No
   * interpolador FP16 do Mali-400/450, UVs acima de 0.5 podem arredondar o
   * ultimo fragmento para o texel vizinho transparente; o discard deixa a cor
   * de limpeza aparecer como uma grade. Recuar as quatro bordas 0.25 texel
   * mantem toda amostra dentro do tile e preserva a distribuicao 2x exata:
   * cada texel fonte continua cobrindo dois pixels da saida.
   *
   * O ajuste nao e' um workaround global: tamanho, target e layout completo
   * do lote sao validados, uma copia e' entregue ao driver, e quads especiais
   * (UI, texto, fullscreen ou UV com outro span) permanecem byte a byte iguais.
   */
  if (target == 0x8892u /* GL_ARRAY_BUFFER */ &&
      size == TS_TILEMAP_VBO_BYTES && data) {
    void *copy = malloc(TS_TILEMAP_VBO_BYTES);
    if (copy) {
      size_t fixed;
      memcpy(copy, data, TS_TILEMAP_VBO_BYTES);
      fixed = ts_tilemap_uv_fix_in_place(copy, TS_TILEMAP_VBO_BYTES);
      if (ts_tilemap_uv_is_canonical_batch(fixed)) {
        if (!uv_fix_logged) {
          logPrintf("[tilemap] UV inset 0.25/1024 aplicado a %u quads validados\n",
                    (unsigned)fixed);
          uv_fix_logged = 1;
        }
        glBufferData(target, size, copy, usage);
        free(copy);
        return;
      }
      free(copy);
    }
  }
  glBufferData(target, size, data, usage);
}

/* Se a engine resolver GL por eglGetProcAddress em vez da tabela de import,
 * o wrapper SOFTFP tem que valer do mesmo jeito — senao o glClearColor volta a
 * receber lixo por um caminho lateral. */
void *ts_gl_proc_override(const char *name) {
  if (!name) return NULL;
  static const DynLibFunction sf[] = {
      TS("glBlendColor", asm2_sf_glBlendColor),
      TS("glClearColor", asm2_sf_glClearColor),
      TS("glClearDepthf", asm2_sf_glClearDepthf),
      TS("glDepthRangef", asm2_sf_glDepthRangef),
      TS("glLineWidth", asm2_sf_glLineWidth),
      TS("glPolygonOffset", asm2_sf_glPolygonOffset),
      TS("glSampleCoverage", asm2_sf_glSampleCoverage),
      TS("glTexParameterf", asm2_sf_glTexParameterf),
      TS("glUniform1f", asm2_sf_glUniform1f),
    TS("glUniform1fv", ts_glUniform1fv),
    TS("glUniform2fv", ts_glUniform2fv),
      TS("glVertexAttrib4f", asm2_sf_glVertexAttrib4f),
      TS("glGetString", ts_glGetString),
      TS("glBufferData", ts_glBufferData),
      TS("glVertexAttribPointer", ts_glVertexAttribPointer),
      TS("glCreateShader", ts_glCreateShader),
      TS("glShaderSource", ts_glShaderSource),
      TS("glCompileShader", ts_glCompileShader),
      TS("glLinkProgram", ts_glLinkProgram),
      TS("glUniformMatrix4fv", ts_glUniformMatrix4fv),
  };
  for (unsigned i = 0; i < sizeof(sf) / sizeof(sf[0]); i++)
    if (strcmp(name, sf[i].symbol) == 0) return (void *)sf[i].func;
  return NULL;
}

DynLibFunction ts_imports[] = {
    /* ---------------- bionic: dados e log ---------------- */
    TS("_ctype_", &asm2_ctype_ptr),
    TS("_tolower_tab_", &asm2_tolower_ptr),
    TS("_toupper_tab_", &asm2_toupper_ptr),
    TS("__page_size", &asm2_page_size),
    TS("__stack_chk_guard", &asm2_stack_chk_guard),
    TS("__sF", &asm2_bionic_sF[0][0]),
    TS("__errno", asm2_errno),
    TS("__android_log_print", asm2_android_log_print),
    TS("__android_log_vprint", asm2_android_log_vprint),
    TS("__assert2", asm2_assert2),
    TS("__gnu_Unwind_Find_exidx", asm2_unwind_find_exidx),

    /* ---------------- bionic: FILE de 84 bytes ---------------- */
    TS("clearerr", asm2_clearerr),
    TS("fclose", asm2_fclose),
    TS("fdopen", asm2_fdopen),
    TS("fflush", asm2_fflush),
    TS("fgets", asm2_fgets),
    TS("fopen", asm2_fopen),
    TS("fprintf", asm2_fprintf),
    TS("fputc", asm2_fputc),
    TS("fputs", asm2_fputs),
    TS("fread", asm2_fread),
    TS("fseek", asm2_fseek),
    TS("fseeko", asm2_fseeko),
    TS("ftell", asm2_ftell),
    TS("ftello", asm2_ftello),
    TS("fwrite", asm2_fwrite),
    TS("getc", asm2_getc),
    TS("getwc", asm2_getwc),
    TS("putc", asm2_putc),
    TS("putwc", asm2_putwc),
    TS("setvbuf", asm2_setvbuf),
    TS("ungetc", asm2_ungetc),
    TS("ungetwc", asm2_ungetwc),
    TS("vfprintf", asm2_vfprintf),

    /* ---------------- caminhos e structs de sistema ---------------- */
    TS("access", asm2_access),
    TS("chdir", asm2_chdir),
    TS("chmod", asm2_chmod),
    TS("getcwd", asm2_getcwd),
    TS("mkdir", asm2_mkdir),
    TS("mkstemp", asm2_mkstemp),
    TS("open", ts_open),
    TS("opendir", asm2_opendir),
    TS("readdir", asm2_readdir),
    TS("remove", asm2_remove),
    TS("rename", asm2_rename),
    TS("rmdir", asm2_rmdir),
    TS("stat", asm2_stat),
    TS("fstat", asm2_fstat),
    TS("statfs", asm2_statfs),
    TS("unlink", asm2_unlink),
    TS("sigaction", ts_sigaction),
    TS("strerror_r", asm2_strerror_r),
    TS("sysconf", ts_sysconf),
    TS("setjmp", asm2_setjmp),
    TS("longjmp", asm2_longjmp),
    TS("regcomp", ts_regcomp),
    TS("regexec", ts_regexec),
    TS("regfree", ts_regfree),
    TS("read", ts_read),

    /* ---------------- pthread/sem: objetos bionic sao menores -------- */
    TS("pthread_attr_init", asm2_pthread_attr_init),
    TS("pthread_attr_destroy", asm2_pthread_attr_destroy),
    TS("pthread_attr_getdetachstate", asm2_pthread_attr_getdetachstate),
    TS("pthread_attr_setdetachstate", asm2_pthread_attr_setdetachstate),
    TS("pthread_attr_getstacksize", asm2_pthread_attr_getstacksize),
    TS("pthread_attr_setstacksize", asm2_pthread_attr_setstacksize),
    TS("pthread_mutexattr_init", asm2_pthread_mutexattr_init),
    TS("pthread_mutexattr_destroy", asm2_pthread_mutexattr_destroy),
    TS("pthread_mutexattr_gettype", asm2_pthread_mutexattr_gettype),
    TS("pthread_mutexattr_settype", asm2_pthread_mutexattr_settype),
    TS("pthread_mutex_init", asm2_pthread_mutex_init),
    TS("pthread_mutex_destroy", asm2_pthread_mutex_destroy),
    TS("pthread_mutex_lock", asm2_pthread_mutex_lock),
    TS("pthread_mutex_trylock", asm2_pthread_mutex_trylock),
    TS("pthread_mutex_unlock", asm2_pthread_mutex_unlock),
    TS("pthread_cond_init", asm2_pthread_cond_init),
    TS("pthread_cond_destroy", asm2_pthread_cond_destroy),
    TS("pthread_cond_wait", asm2_pthread_cond_wait),
    TS("pthread_cond_timedwait", asm2_pthread_cond_timedwait),
    TS("pthread_cond_signal", asm2_pthread_cond_signal),
    TS("pthread_cond_broadcast", asm2_pthread_cond_broadcast),
    TS("pthread_create", asm2_pthread_create),
    TS("pthread_detach", asm2_pthread_detach),
    TS("pthread_equal", asm2_pthread_equal),
    TS("pthread_getschedparam", asm2_pthread_getschedparam),
    TS("pthread_setschedparam", asm2_pthread_setschedparam),
    TS("pthread_getspecific", asm2_pthread_getspecific),
    TS("pthread_setspecific", asm2_pthread_setspecific),
    TS("pthread_join", asm2_pthread_join),
    TS("pthread_key_create", asm2_pthread_key_create),
    TS("pthread_key_delete", asm2_pthread_key_delete),
    TS("pthread_self", asm2_pthread_self),
    TS("pthread_once", asm2_pthread_once),
    TS("sem_init", asm2_sem_init),
    TS("sem_destroy", asm2_sem_destroy),
    TS("sem_post", asm2_sem_post),
    TS("sem_trywait", asm2_sem_trywait),
    TS("sem_wait", asm2_sem_wait),
    TS("sem_getvalue", asm2_sem_getvalue),
    TS("sem_timedwait", asm2_sem_timedwait),
    TS("pthread_setname_np", ts_pthread_setname_np),

    /* ---------------- FRONTEIRA SOFTFP: libm ---------------- */
    TS("acos", asm2_sf_acos),
    TS("acosf", asm2_sf_acosf),
    TS("asin", asm2_sf_asin),
    TS("asinf", asm2_sf_asinf),
    TS("atan", asm2_sf_atan),
    TS("atanf", asm2_sf_atanf),
    TS("atan2", asm2_sf_atan2),
    TS("atan2f", asm2_sf_atan2f),
    TS("atof", asm2_sf_atof),
    TS("ceil", asm2_sf_ceil),
    TS("ceilf", asm2_sf_ceilf),
    TS("cos", asm2_sf_cos),
    TS("cosf", asm2_sf_cosf),
    TS("difftime", asm2_sf_difftime),
    TS("exp", asm2_sf_exp),
    TS("expf", asm2_sf_expf),
    TS("floor", asm2_sf_floor),
    TS("floorf", asm2_sf_floorf),
    TS("fmod", asm2_sf_fmod),
    TS("fmodf", asm2_sf_fmodf),
    TS("frexp", asm2_sf_frexp),
    TS("ldexp", asm2_sf_ldexp),
    TS("log", asm2_sf_log),
    TS("logf", asm2_sf_logf),
    TS("log10", asm2_sf_log10),
    TS("log10f", asm2_sf_log10f),
    TS("lrintf", asm2_sf_lrintf),
    TS("modff", asm2_sf_modff),
    TS("pow", asm2_sf_pow),
    TS("powf", asm2_sf_powf),
    TS("rint", asm2_sf_rint),
    TS("sin", asm2_sf_sin),
    TS("sinf", asm2_sf_sinf),
    TS("sinh", asm2_sf_sinh),
    TS("sqrt", asm2_sf_sqrt),
    TS("sqrtf", asm2_sf_sqrtf),
    TS("strtod", asm2_sf_strtod),
    TS("strtof", asm2_sf_strtof),
    TS("tan", asm2_sf_tan),
    TS("tanf", asm2_sf_tanf),

    /* ---------------- FRONTEIRA SOFTFP: GLES2 com float -------------- */
    TS("glBlendColor", asm2_sf_glBlendColor),
    TS("glClearColor", asm2_sf_glClearColor),
    TS("glClearDepthf", asm2_sf_glClearDepthf),
    TS("glDepthRangef", asm2_sf_glDepthRangef),
    TS("glLineWidth", asm2_sf_glLineWidth),
    TS("glPolygonOffset", asm2_sf_glPolygonOffset),
    TS("glSampleCoverage", asm2_sf_glSampleCoverage),
    TS("glTexParameterf", asm2_sf_glTexParameterf),
    TS("glUniform1f", asm2_sf_glUniform1f),
    TS("glUniform1fv", ts_glUniform1fv),
    TS("glGetUniformLocation", ts_glGetUniformLocation),
    TS("glUniform2fv", ts_glUniform2fv),
    TS("glVertexAttrib4f", asm2_sf_glVertexAttrib4f),
    TS("glGetString", ts_glGetString),
    TS("glBufferData", ts_glBufferData),
    TS("glVertexAttribPointer", ts_glVertexAttribPointer),
    TS("glCreateShader", ts_glCreateShader),
    TS("glShaderSource", ts_glShaderSource),
    TS("glCompileShader", ts_glCompileShader),
    TS("glLinkProgram", ts_glLinkProgram),
    TS("glUniformMatrix4fv", ts_glUniformMatrix4fv),

    /* ---------------- libgcc AEABI ---------------- */
    TS("__aeabi_idiv", __aeabi_idiv),
    TS("__aeabi_idivmod", __aeabi_idivmod),
    TS("__aeabi_uidiv", __aeabi_uidiv),
    TS("__aeabi_uidivmod", __aeabi_uidivmod),
    TS("__aeabi_uldivmod", __aeabi_uldivmod),
    TS("__aeabi_ldivmod", __aeabi_ldivmod),
    TS("__aeabi_d2lz", __aeabi_d2lz),
    TS("__aeabi_d2ulz", __aeabi_d2ulz),
    TS("__aeabi_ul2d", __aeabi_ul2d),
    TS("__aeabi_l2d", __aeabi_l2d),
    TS("__aeabi_unwind_cpp_pr0", __aeabi_unwind_cpp_pr0),
    TS("__aeabi_unwind_cpp_pr1", __aeabi_unwind_cpp_pr1),
    TS("__aeabi_unwind_cpp_pr2", __aeabi_unwind_cpp_pr2),

    /* ---------------- C++ runtime ---------------- */
    TS("_Znwj", ts_op_new),
    TS("_Znaj", ts_op_new),
    TS("_ZdlPv", ts_op_delete),
    TS("_ZdaPv", ts_op_delete),
    TS("__cxa_pure_virtual", ts_pure_virtual),

    /* ---------------- EGL -> egl_shim (GLES2 via SDL2, Mali) --------- */
    TS("eglGetDisplay", egl_shim_GetDisplay),
    TS("eglInitialize", egl_shim_Initialize),
    TS("eglTerminate", egl_shim_Terminate),
    TS("eglChooseConfig", egl_shim_ChooseConfig),
    TS("eglCreateWindowSurface", egl_shim_CreateWindowSurface),
    TS("eglCreatePbufferSurface", egl_shim_CreatePbufferSurface),
    TS("eglCreateContext", egl_shim_CreateContext),
    TS("eglDestroyContext", egl_shim_DestroyContext),
    TS("eglDestroySurface", egl_shim_DestroySurface),
    TS("eglGetConfigAttrib", egl_shim_GetConfigAttrib),
    TS("eglQuerySurface", egl_shim_QuerySurface),
    TS("eglQueryString", egl_shim_QueryString),
    TS("eglGetError", egl_shim_GetError),
    TS("eglGetProcAddress", egl_shim_GetProcAddress),
    TS("eglMakeCurrent", egl_shim_MakeCurrent),
    TS("eglSwapBuffers", egl_shim_SwapBuffers),
    TS("eglSwapInterval", egl_shim_SwapInterval),
    TS("eglBindAPI", egl_shim_BindAPI),
    TS("eglGetCurrentContext", egl_shim_GetCurrentContext),
    TS("eglGetCurrentSurface", egl_shim_GetCurrentSurface),
    TS("eglSurfaceAttrib", egl_shim_SurfaceAttrib),

    /* ---------------- NDK: assets (KTD-2 do plano) ---------------- */
    TS("AAssetManager_open", AAssetManager_open),
    TS("AAssetManager_fromJava", AAssetManager_fromJava),
    TS("AAsset_read", AAsset_read),
    TS("AAsset_seek", AAsset_seek),
    TS("AAsset_getLength", AAsset_getLength),
    TS("AAsset_getRemainingLength", AAsset_getRemainingLength),
    TS("AAsset_getBuffer", AAsset_getBuffer),
    TS("AAsset_openFileDescriptor", AAsset_openFileDescriptor),
    TS("AAsset_close", AAsset_close),

    /* ---------------- NDK: looper / input / janela / config ---------- */
    TS("ALooper_prepare", ALooper_prepare),
    TS("ALooper_addFd", ALooper_addFd),
    TS("ALooper_removeFd", ALooper_removeFd),
    TS("ALooper_pollAll", ALooper_pollAll),
    TS("ALooper_pollOnce", ALooper_pollOnce),
    TS("ALooper_forThread", ALooper_forThread),
    TS("ALooper_acquire", ALooper_acquire),
    TS("ALooper_release", ALooper_release),
    TS("ALooper_wake", ALooper_wake),
    TS("AInputQueue_attachLooper", AInputQueue_attachLooper),
    TS("AInputQueue_detachLooper", AInputQueue_detachLooper),
    TS("AInputQueue_hasEvents", AInputQueue_hasEvents),
    TS("AInputQueue_getEvent", AInputQueue_getEvent),
    TS("AInputQueue_preDispatchEvent", AInputQueue_preDispatchEvent),
    TS("AInputQueue_finishEvent", AInputQueue_finishEvent),
    TS("AInputEvent_getType", AInputEvent_getType),
    TS("AInputEvent_getSource", AInputEvent_getSource),
    TS("AInputEvent_getDeviceId", AInputEvent_getDeviceId),
    TS("AKeyEvent_getAction", AKeyEvent_getAction),
    TS("AKeyEvent_getKeyCode", AKeyEvent_getKeyCode),
    TS("AKeyEvent_getFlags", AKeyEvent_getFlags),
    TS("AKeyEvent_getMetaState", AKeyEvent_getMetaState),
    TS("AKeyEvent_getRepeatCount", AKeyEvent_getRepeatCount),
    TS("AMotionEvent_getAction", AMotionEvent_getAction),
    TS("AMotionEvent_getAxisValue", AMotionEvent_getAxisValue),
    TS("AMotionEvent_getX", AMotionEvent_getX),
    TS("AMotionEvent_getY", AMotionEvent_getY),
    TS("AMotionEvent_getPointerCount", AMotionEvent_getPointerCount),
    TS("AMotionEvent_getPointerId", AMotionEvent_getPointerId),
    TS("AMotionEvent_getButtonState", AMotionEvent_getButtonState),
    TS("ANativeWindow_setBuffersGeometry", ANativeWindow_setBuffersGeometry),
    TS("ANativeWindow_getWidth", ANativeWindow_getWidth),
    TS("ANativeWindow_getHeight", ANativeWindow_getHeight),
    TS("ANativeWindow_getFormat", ANativeWindow_getFormat),
    TS("ANativeWindow_acquire", ANativeWindow_acquire),
    TS("ANativeWindow_release", ANativeWindow_release),
    TS("ANativeActivity_finish", ANativeActivity_finish),
    TS("ANativeActivity_setWindowFlags", ANativeActivity_setWindowFlags),
    TS("AConfiguration_new", AConfiguration_new),
    TS("AConfiguration_delete", AConfiguration_delete),
    TS("AConfiguration_fromAssetManager", AConfiguration_fromAssetManager),
    TS("AConfiguration_setLocale", AConfiguration_setLocale),
    TS("AConfiguration_getLanguage", AConfiguration_getLanguage),
    TS("AConfiguration_getCountry", AConfiguration_getCountry),
    TS("AConfiguration_getDensity", AConfiguration_getDensity),
    TS("AConfiguration_getOrientation", AConfiguration_getOrientation),
    TS("AConfiguration_getScreenSize", AConfiguration_getScreenSize),

    /* ---------------- sensores: existem, e nao ha nenhum ------------- */
    TS("ASensorManager_getInstance", asm2_ASensorManager_getInstance),
    TS("ASensorManager_getDefaultSensor", asm2_ASensorManager_getDefaultSensor),
    TS("ASensorManager_createEventQueue", asm2_ASensorManager_createEventQueue),
    TS("ASensorEventQueue_enableSensor", asm2_ASensorEventQueue_enableSensor),
    TS("ASensorEventQueue_disableSensor", asm2_ASensorEventQueue_disableSensor),
    TS("ASensorEventQueue_getEvents", asm2_ASensorEventQueue_getEvents),
    TS("ASensorEventQueue_setEventRate", asm2_ASensorEventQueue_setEventRate),

    /* ---------------- OpenSL ES (saida do FMOD Ex) ---------------- */
    TS("dlopen", ts_dlopen),
    TS("dlsym", ts_dlsym),
    TS("dlclose", ts_dlclose),
    TS("slCreateEngine", slCreateEngine_shim),
    TS("SL_IID_ENGINE", &sl_IID_ENGINE),
    TS("SL_IID_PLAY", &sl_IID_PLAY),
    TS("SL_IID_VOLUME", &sl_IID_VOLUME),
    TS("SL_IID_BUFFERQUEUE", &sl_IID_BUFFERQUEUE),
    TS("SL_IID_ANDROIDSIMPLEBUFFERQUEUE", &sl_IID_BUFFERQUEUE),


    /* ---- FMOD: capturar o System para bombear o update na espera ---- */
    TS("FMOD_System_Create", ts_FMOD_System_Create),
    TS(TS_SYM_STREAM, ts_fmod_createStream),
    TS(TS_SYM_SOUND, ts_fmod_createSound),
    TS(TS_SYM_SETFS, ts_fmod_setFileSystem),
    TS(TS_SYM_INIT, ts_fmod_init),

    /* ---------------- Amazon GameCircle: periferico, "ok" ----------- */
    TS("_ZN11AmazonGames17WhispersyncClient11getGameDataEv", ag_null),
    TS("_ZN11AmazonGames27AchievementsClientInterface14updateProgressEPKcfPNS_"
       "17IUpdateProgressCbEi", ag_noop),
    TS("_ZN11AmazonGames27AchievementsClientInterface23showAchievementsOverlay"
       "Ev", ag_noop),
    TS("_ZN11AmazonGames27LeaderboardsClientInterface11submitScoreEPKcxPNS_"
       "25ILeaderboardSubmitScoreCbEi", ag_noop),
    TS("_ZN11AmazonGames9ICallbackD2Ev", ag_noop),
};

const int ts_imports_count = (int)(sizeof(ts_imports) / sizeof(ts_imports[0]));
