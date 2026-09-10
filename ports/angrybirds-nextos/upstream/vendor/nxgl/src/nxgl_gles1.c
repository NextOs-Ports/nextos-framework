/* SPDX-License-Identifier: GPL-3.0-only */
/* Implementacao do resolvedor GLES1. Ver nxgl_gles1.h para o motivo. */
#define NXGL_GLES1_NO_REDIRECT 1

#include "nxgl_gles1.h"

#include <dlfcn.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Ordem de resolucao: da fonte mais COERENTE com o contexto vivo para a menos
 * coerente -- nunca pela especificidade do nome do arquivo.
 *
 * Medido no R36S com dArkOS (Mali-Bifrost G31), os SONAMEs estao CRUZADOS:
 *
 *   libGLESv1_CM.so.1 -> libGLESv1_CM.so.1.2.0  Mesa de 198 KB, SEM driver
 *   libGLESv1_CM.so   -> libmali-bifrost-g31-*  o blob real, 40 MB
 *   libmali.so        -> o mesmo blob de 40 MB
 *
 * Uma cadeia que preferisse o nome versionado escolheria a biblioteca morta:
 * o contexto aceita toda chamada, glGetString(GL_RENDERER) devolve NULL e nada
 * e' desenhado, com audio e input vivos. Por isso os nomes de BLOB vem antes,
 * e antes deles vem as duas fontes coerentes por construcao: o simbolo que ja'
 * esta' no processo (mesmos objetos que a SDL carregou) e o eglGetProcAddress
 * do EGL que a SDL efetivamente abriu. */
static const char *const k_providers[] = {
  "libmali.so",
  "libMali.so",
  "libGLES_mali.so",
  "libGLESv1_CM.so",
  "libmali.so.1",
  "libGLESv1_CM.so.1",
};

void (*nxgl_gles1_pfn_glAlphaFunc)(GLenum func, GLclampf ref);
void (*nxgl_gles1_pfn_glBindBuffer)(GLenum target, GLuint buffer);
void (*nxgl_gles1_pfn_glBindTexture)(GLenum target, GLuint texture);
void (*nxgl_gles1_pfn_glBlendFunc)(GLenum sfactor, GLenum dfactor);
void (*nxgl_gles1_pfn_glClear)(GLbitfield mask);
void (*nxgl_gles1_pfn_glClearColor)(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
void (*nxgl_gles1_pfn_glColor4f)(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
void (*nxgl_gles1_pfn_glColor4ub)(GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha);
void (*nxgl_gles1_pfn_glColorPointer)(GLint size, GLenum type, GLsizei stride, const void *pointer);
void (*nxgl_gles1_pfn_glCullFace)(GLenum mode);
void (*nxgl_gles1_pfn_glDeleteTextures)(GLsizei n, const GLuint *textures);
void (*nxgl_gles1_pfn_glDepthFunc)(GLenum func);
void (*nxgl_gles1_pfn_glDepthMask)(GLboolean flag);
void (*nxgl_gles1_pfn_glDisable)(GLenum cap);
void (*nxgl_gles1_pfn_glDisableClientState)(GLenum array);
void (*nxgl_gles1_pfn_glDrawArrays)(GLenum mode, GLint first, GLsizei count);
void (*nxgl_gles1_pfn_glEnable)(GLenum cap);
void (*nxgl_gles1_pfn_glEnableClientState)(GLenum array);
void (*nxgl_gles1_pfn_glFogf)(GLenum pname, GLfloat param);
void (*nxgl_gles1_pfn_glFogfv)(GLenum pname, const GLfloat *params);
void (*nxgl_gles1_pfn_glGenTextures)(GLsizei n, GLuint *textures);
void (*nxgl_gles1_pfn_glGetBooleanv)(GLenum pname, GLboolean *params);
GLenum (*nxgl_gles1_pfn_glGetError)(void);
void (*nxgl_gles1_pfn_glGetFloatv)(GLenum pname, GLfloat *params);
void (*nxgl_gles1_pfn_glGetIntegerv)(GLenum pname, GLint *params);
void (*nxgl_gles1_pfn_glGetPointerv)(GLenum pname, void **params);
void (*nxgl_gles1_pfn_glLightfv)(GLenum light, GLenum pname, const GLfloat *params);
void (*nxgl_gles1_pfn_glLineWidth)(GLfloat width);
void (*nxgl_gles1_pfn_glLoadIdentity)(void);
void (*nxgl_gles1_pfn_glLoadMatrixf)(const GLfloat *m);
void (*nxgl_gles1_pfn_glMaterialfv)(GLenum face, GLenum pname, const GLfloat *params);
void (*nxgl_gles1_pfn_glMatrixMode)(GLenum mode);
void (*nxgl_gles1_pfn_glMultMatrixf)(const GLfloat *m);
void (*nxgl_gles1_pfn_glNormalPointer)(GLenum type, GLsizei stride, const void *pointer);
void (*nxgl_gles1_pfn_glOrthof)(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f);
void (*nxgl_gles1_pfn_glPopMatrix)(void);
void (*nxgl_gles1_pfn_glPushMatrix)(void);
void (*nxgl_gles1_pfn_glScissor)(GLint x, GLint y, GLsizei width, GLsizei height);
void (*nxgl_gles1_pfn_glTexCoordPointer)(GLint size, GLenum type, GLsizei stride, const void *pointer);
void (*nxgl_gles1_pfn_glTexImage2D)(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels);
void (*nxgl_gles1_pfn_glTexParameteri)(GLenum target, GLenum pname, GLint param);
void (*nxgl_gles1_pfn_glTexSubImage2D)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels);
void (*nxgl_gles1_pfn_glTranslatef)(GLfloat x, GLfloat y, GLfloat z);
void (*nxgl_gles1_pfn_glVertexPointer)(GLint size, GLenum type, GLsizei stride, const void *pointer);
void (*nxgl_gles1_pfn_glViewport)(GLint x, GLint y, GLsizei width, GLsizei height);
void (*nxgl_gles1_pfn_glReadPixels)(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels);
const GLubyte * (*nxgl_gles1_pfn_glGetString)(GLenum name);

struct nxgl_gles1_entry {
  const char *name;
  void **slot;
};

static const struct nxgl_gles1_entry k_entries[] = {
  { "glAlphaFunc", (void **)&nxgl_gles1_pfn_glAlphaFunc },
  { "glBindBuffer", (void **)&nxgl_gles1_pfn_glBindBuffer },
  { "glBindTexture", (void **)&nxgl_gles1_pfn_glBindTexture },
  { "glBlendFunc", (void **)&nxgl_gles1_pfn_glBlendFunc },
  { "glClear", (void **)&nxgl_gles1_pfn_glClear },
  { "glClearColor", (void **)&nxgl_gles1_pfn_glClearColor },
  { "glColor4f", (void **)&nxgl_gles1_pfn_glColor4f },
  { "glColor4ub", (void **)&nxgl_gles1_pfn_glColor4ub },
  { "glColorPointer", (void **)&nxgl_gles1_pfn_glColorPointer },
  { "glCullFace", (void **)&nxgl_gles1_pfn_glCullFace },
  { "glDeleteTextures", (void **)&nxgl_gles1_pfn_glDeleteTextures },
  { "glDepthFunc", (void **)&nxgl_gles1_pfn_glDepthFunc },
  { "glDepthMask", (void **)&nxgl_gles1_pfn_glDepthMask },
  { "glDisable", (void **)&nxgl_gles1_pfn_glDisable },
  { "glDisableClientState", (void **)&nxgl_gles1_pfn_glDisableClientState },
  { "glDrawArrays", (void **)&nxgl_gles1_pfn_glDrawArrays },
  { "glEnable", (void **)&nxgl_gles1_pfn_glEnable },
  { "glEnableClientState", (void **)&nxgl_gles1_pfn_glEnableClientState },
  { "glFogf", (void **)&nxgl_gles1_pfn_glFogf },
  { "glFogfv", (void **)&nxgl_gles1_pfn_glFogfv },
  { "glGenTextures", (void **)&nxgl_gles1_pfn_glGenTextures },
  { "glGetBooleanv", (void **)&nxgl_gles1_pfn_glGetBooleanv },
  { "glGetError", (void **)&nxgl_gles1_pfn_glGetError },
  { "glGetFloatv", (void **)&nxgl_gles1_pfn_glGetFloatv },
  { "glGetIntegerv", (void **)&nxgl_gles1_pfn_glGetIntegerv },
  { "glGetPointerv", (void **)&nxgl_gles1_pfn_glGetPointerv },
  { "glLightfv", (void **)&nxgl_gles1_pfn_glLightfv },
  { "glLineWidth", (void **)&nxgl_gles1_pfn_glLineWidth },
  { "glLoadIdentity", (void **)&nxgl_gles1_pfn_glLoadIdentity },
  { "glLoadMatrixf", (void **)&nxgl_gles1_pfn_glLoadMatrixf },
  { "glMaterialfv", (void **)&nxgl_gles1_pfn_glMaterialfv },
  { "glMatrixMode", (void **)&nxgl_gles1_pfn_glMatrixMode },
  { "glMultMatrixf", (void **)&nxgl_gles1_pfn_glMultMatrixf },
  { "glNormalPointer", (void **)&nxgl_gles1_pfn_glNormalPointer },
  { "glOrthof", (void **)&nxgl_gles1_pfn_glOrthof },
  { "glPopMatrix", (void **)&nxgl_gles1_pfn_glPopMatrix },
  { "glPushMatrix", (void **)&nxgl_gles1_pfn_glPushMatrix },
  { "glScissor", (void **)&nxgl_gles1_pfn_glScissor },
  { "glTexCoordPointer", (void **)&nxgl_gles1_pfn_glTexCoordPointer },
  { "glTexImage2D", (void **)&nxgl_gles1_pfn_glTexImage2D },
  { "glTexParameteri", (void **)&nxgl_gles1_pfn_glTexParameteri },
  { "glTexSubImage2D", (void **)&nxgl_gles1_pfn_glTexSubImage2D },
  { "glTranslatef", (void **)&nxgl_gles1_pfn_glTranslatef },
  { "glVertexPointer", (void **)&nxgl_gles1_pfn_glVertexPointer },
  { "glViewport", (void **)&nxgl_gles1_pfn_glViewport },
  { "glReadPixels", (void **)&nxgl_gles1_pfn_glReadPixels },
  { "glGetString", (void **)&nxgl_gles1_pfn_glGetString },
};

static char g_provider[NXGL_GLES1_PROVIDER_MAX];
static char g_liveness[16];
static int g_done;
static nxgl_gles1_resolver_fn g_primary_resolver;

/* V3: rastro "nome:razao" na ORDEM de medicao (ver nxgl_gles1.h). O rastro
 * termina no candidato escolhido: o que vem depois dele nao foi invocado, e
 * a ausencia no rastro e' exatamente a prova disso. */
static char g_trace[NXGL_GLES1_CANDIDATES_MAX];

static void trace_append(const char *name, const char *reason) {
  size_t used = strlen(g_trace);

  if (used >= sizeof(g_trace) - 1u) {
    return;
  }
  snprintf(g_trace + used, sizeof(g_trace) - used, "%s%s:%s",
           used > 0u ? " " : "", name, reason);
}

typedef void *(*nxgl_gles1_eglGetProcAddress_fn)(const char *);

void nxgl_gles1_set_primary_resolver(nxgl_gles1_resolver_fn resolver) {
  g_primary_resolver = resolver;
}

/* 0.2.14: selecao por CONJUNTO com prova de vida, nunca por ordem de nome.
 *
 * Medido em 23/08/2026 abrindo a imagem oficial do ROCKNIX RK3566: la' os
 * SONAMEs estao cruzados no sentido OPOSTO ao do dArkOS -- libmali.so e' um
 * blob kbase ORFAO (a sessao viva e' Mesa/Panfrost) e libGLESv1_CM.so.1 e' o
 * dispatch VIVO. A cadeia por nome (blob primeiro, o conserto do dArkOS)
 * resolvia 47/47 do provedor morto: jogo inteiro em no-ops, tela preta com
 * som. Nenhuma ordem fixa acerta nas duas firmwares; so' MEDICAO.
 *
 * O criterio vivo: com o contexto corrente, o glGetString(GL_RENDERER) DO
 * PROPRIO candidato responde non-nulo. E' a mesma medicao que o frame proof
 * usa (provada em campo nas duas firmwares, sem crash em provedor morto).
 *
 * Ordem de candidatos, cada um resolvendo TODOS os pontos de entrada de uma
 * unica fonte (fim da mistura de origens):
 *   0. resolvedor primario injetado pelo port (SDL_GL_GetProcAddress depois
 *      de SDL_GL_CreateContext -- coerente com o contexto por construcao);
 *   1. RTLD_DEFAULT;  2. eglGetProcAddress do processo;  3+. a cadeia dlopen.
 * O primeiro candidato COMPLETO e VIVO vence. Sem nenhum vivo, o primeiro
 * completo vence com liveness=dead (comportamento v1 preservado para quem
 * inicializa sem contexto corrente); sem nenhum completo, cai na montagem
 * mista por simbolo do v1, como ultimo recurso. */

#define NXGL_GLES1_ENTRY_COUNT (sizeof(k_entries) / sizeof(k_entries[0]))
#define NXGL_GLES1_GETSTRING_INDEX (NXGL_GLES1_ENTRY_COUNT - 1u)

typedef const GLubyte *(*nxgl_gles1_get_string_fn)(GLenum name);

static int staging_is_live(void *const *staging) {
  nxgl_gles1_get_string_fn get_string =
      (nxgl_gles1_get_string_fn)staging[NXGL_GLES1_GETSTRING_INDEX];
  const GLubyte *renderer;

  if (get_string == NULL) {
    return 0;
  }
  renderer = get_string(0x1F01 /* GL_RENDERER */);
  return renderer != NULL && renderer[0] != '\0';
}

static unsigned resolve_set_from_dlsym(void *handle, void **staging) {
  unsigned resolved = 0;
  size_t i;

  for (i = 0; i < NXGL_GLES1_ENTRY_COUNT; ++i) {
    staging[i] = dlsym(handle, k_entries[i].name);
    if (staging[i] != NULL) {
      ++resolved;
    }
  }
  return resolved;
}

static unsigned resolve_set_from_callback(void *(*resolver)(const char *),
                                          void **staging) {
  unsigned resolved = 0;
  size_t i;

  for (i = 0; i < NXGL_GLES1_ENTRY_COUNT; ++i) {
    staging[i] = resolver(k_entries[i].name);
    if (staging[i] != NULL) {
      ++resolved;
    }
  }
  return resolved;
}

static void commit_staging(void *const *staging) {
  size_t i;

  for (i = 0; i < NXGL_GLES1_ENTRY_COUNT; ++i) {
    *k_entries[i].slot = staging[i];
  }
}

struct nxgl_gles1_candidate {
  char name[NXGL_GLES1_PROVIDER_MAX];
  void *staging[NXGL_GLES1_ENTRY_COUNT];
};

int nxgl_gles1_init(nxgl_gles1_receipt *receipt) {
  void *handles[sizeof(k_providers) / sizeof(k_providers[0])];
  static struct nxgl_gles1_candidate candidate; /* fora da pilha do port */
  static struct nxgl_gles1_candidate first_complete;
  nxgl_gles1_eglGetProcAddress_fn get_proc;
  const char *missing = "";
  unsigned resolved = 0;
  unsigned rejected_dead = 0;
  int have_complete = 0;
  int committed = 0;
  size_t provider_count = sizeof(k_providers) / sizeof(k_providers[0]);
  size_t entry_count = NXGL_GLES1_ENTRY_COUNT;
  size_t i;

  if (g_done) {
    if (receipt != NULL) {
      memset(receipt, 0, sizeof(*receipt));
      snprintf(receipt->provider, sizeof(receipt->provider), "%s", g_provider);
      receipt->resolved = (unsigned)entry_count;
      receipt->total = (unsigned)entry_count;
      snprintf(receipt->text, sizeof(receipt->text),
               "GLES1: provider=%s resolved=%u/%u liveness=%s (cache)",
               g_provider, receipt->resolved, receipt->total, g_liveness);
      snprintf(receipt->candidates, sizeof(receipt->candidates), "%s",
               g_trace);
    }
    return 0;
  }

  for (i = 0; i < provider_count; ++i) {
    handles[i] = dlopen(k_providers[i], RTLD_NOW | RTLD_LOCAL);
  }
  get_proc = (nxgl_gles1_eglGetProcAddress_fn)dlsym(RTLD_DEFAULT,
                                                   "eglGetProcAddress");

  /* Candidatos, na ordem de coerencia. O laco para no primeiro VIVO. */
  for (i = 0; i < 3u + provider_count && !committed; ++i) {
    unsigned count = 0;

    candidate.name[0] = '\0';
    if (i == 0u) {
      if (g_primary_resolver == NULL) {
        continue;
      }
      snprintf(candidate.name, sizeof(candidate.name), "primary-resolver");
      count = resolve_set_from_callback(
          (void *(*)(const char *))g_primary_resolver, candidate.staging);
    } else if (i == 1u) {
      snprintf(candidate.name, sizeof(candidate.name), "RTLD_DEFAULT");
      count = resolve_set_from_dlsym(RTLD_DEFAULT, candidate.staging);
    } else if (i == 2u) {
      if (get_proc == NULL) {
        continue;
      }
      snprintf(candidate.name, sizeof(candidate.name), "eglGetProcAddress");
      count = resolve_set_from_callback(
          (void *(*)(const char *))get_proc, candidate.staging);
    } else {
      if (handles[i - 3u] == NULL) {
        trace_append(k_providers[i - 3u], "absent");
        continue;
      }
      snprintf(candidate.name, sizeof(candidate.name), "%s",
               k_providers[i - 3u]);
      count = resolve_set_from_dlsym(handles[i - 3u], candidate.staging);
    }

    if (count != (unsigned)entry_count) {
      trace_append(candidate.name, "incomplete");
      continue; /* conjunto incompleto nao e' candidato */
    }
    if (!have_complete) {
      have_complete = 1;
      first_complete = candidate;
    }
    if (staging_is_live(candidate.staging)) {
      commit_staging(candidate.staging);
      snprintf(g_provider, sizeof(g_provider), "%s", candidate.name);
      snprintf(g_liveness, sizeof(g_liveness), "ok");
      trace_append(candidate.name, "live");
      resolved = count;
      committed = 1;
    } else {
      trace_append(candidate.name, "dead");
      ++rejected_dead;
    }
  }

  if (!committed && have_complete) {
    /* Nenhum candidato respondeu vivo (ex.: init antes do contexto). O v1
     * teria seguido com o primeiro que resolvesse; preservar isso -- mas o
     * recibo agora DENUNCIA em vez de posar de saudavel. */
    commit_staging(first_complete.staging);
    snprintf(g_provider, sizeof(g_provider), "%s", first_complete.name);
    snprintf(g_liveness, sizeof(g_liveness), "dead");
    resolved = (unsigned)entry_count;
    committed = 1;
  }

  if (!committed) {
    /* Ultimo recurso: a montagem mista por simbolo do v1 (fontes diferentes
     * por entrada). So' acontece quando NENHUMA fonte tem o conjunto todo. */
    const char *first_origin = "";

    for (i = 0; i < entry_count; ++i) {
      const char *origin = "";
      void *addr = dlsym(RTLD_DEFAULT, k_entries[i].name);

      if (addr != NULL) {
        origin = "RTLD_DEFAULT";
      }
      if (addr == NULL && get_proc != NULL) {
        addr = get_proc(k_entries[i].name);
        if (addr != NULL) {
          origin = "eglGetProcAddress";
        }
      }
      if (addr == NULL) {
        size_t j;
        for (j = 0; j < provider_count; ++j) {
          if (handles[j] == NULL) {
            continue;
          }
          addr = dlsym(handles[j], k_entries[i].name);
          if (addr != NULL) {
            origin = k_providers[j];
            break;
          }
        }
      }
      if (addr == NULL) {
        if (missing[0] == '\0') {
          missing = k_entries[i].name;
        }
        continue;
      }
      *k_entries[i].slot = addr;
      if (first_origin[0] == '\0') {
        first_origin = origin;
      }
      ++resolved;
    }
    snprintf(g_provider, sizeof(g_provider), "%s",
             first_origin[0] != '\0' ? first_origin : "nenhum");
    snprintf(g_liveness, sizeof(g_liveness), "mixed");
  }

  if (receipt != NULL) {
    memset(receipt, 0, sizeof(*receipt));
    snprintf(receipt->provider, sizeof(receipt->provider), "%s", g_provider);
    receipt->resolved = resolved;
    receipt->total = (unsigned)entry_count;
    snprintf(receipt->first_missing, sizeof(receipt->first_missing), "%s",
             missing);
    snprintf(receipt->candidates, sizeof(receipt->candidates), "%s", g_trace);
    if (missing[0] != '\0') {
      snprintf(receipt->text, sizeof(receipt->text),
               "GLES1: provider=%s resolved=%u/%u first_missing=%s",
               g_provider, resolved, receipt->total, missing);
    } else {
      snprintf(receipt->text, sizeof(receipt->text),
               "GLES1: provider=%s resolved=%u/%u liveness=%s%s%u",
               g_provider, resolved, receipt->total, g_liveness,
               " rejected-dead=", rejected_dead);
    }
  }

  /* Handles dlopen que nao forneceram o conjunto escolhido sao fechados --
   * v1 vazava todos. No caminho misto (ultimo recurso) todos podem ter
   * cedido simbolos, entao la' nada e' fechado. */
  if (strcmp(g_liveness, "mixed") != 0) {
    for (i = 0; i < provider_count; ++i) {
      if (handles[i] != NULL && strcmp(g_provider, k_providers[i]) != 0) {
        dlclose(handles[i]);
      }
    }
  }

  if (resolved != (unsigned)entry_count) {
    return -1;
  }
  g_done = 1;
  return 0;
}

void *nxgl_gles1_lookup(const char *name) {
  size_t entry_count = sizeof(k_entries) / sizeof(k_entries[0]);
  size_t i;

  if (name == NULL) {
    return NULL;
  }
  for (i = 0; i < entry_count; ++i) {
    if (strcmp(k_entries[i].name, name) == 0) {
      return *k_entries[i].slot;
    }
  }
  return NULL;
}

const char *nxgl_gles1_provider(void) { return g_provider; }

const char *nxgl_gles1_liveness(void) { return g_liveness; }
