/* SPDX-License-Identifier: GPL-3.0-only */
/* Ver nxgl_provider_discovery_adapter.h para o porque deste modulo existir. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "nxgl_provider_discovery_adapter.h"

#include <dirent.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Familias de blob grafico unificado. Sao NOMES DE FAMILIA, nao nomes de
 * aparelho: o que decide e' o dlsym adiante, nunca este prefixo. A lista
 * existe apenas para nao abrir todo .so do sistema. */
static const char *const k_family_prefixes[] = {
    "libmali", "libMali", "libGLES_mali", "libGLESv1_CM", "libEGL",
};

/* Diretorios de biblioteca das CFW usadas. Multiarch e caminhos legados. */
static const char *const k_library_dirs[] = {
    "/usr/lib/aarch64-linux-gnu",
    "/usr/lib/arm-linux-gnueabihf",
    "/usr/lib",
    "/usr/local/lib",
    "/vendor/lib64",
    "/vendor/lib",
};

int nxgl_provider_renderer_is_broken(const char *renderer) {
  return renderer == NULL || renderer[0] == '\0';
}

void nxgl_provider_repair_options_init(nxgl_provider_repair_options *options) {
  if (options == NULL) {
    return;
  }
  memset(options, 0, sizeof(*options));
  options->api_version = NXGL_API_CURRENT_VERSION;
  options->struct_size = sizeof(*options);
}

void nxgl_provider_repair_receipt_init(nxgl_provider_repair_receipt *receipt) {
  if (receipt == NULL) {
    return;
  }
  memset(receipt, 0, sizeof(*receipt));
  receipt->api_version = NXGL_API_CURRENT_VERSION;
  receipt->struct_size = sizeof(*receipt);
  receipt->outcome = NXGL_PROVIDER_REPAIR_NO_ACTION;
  receipt->reason = NXGL_PROVIDER_RECOVERY_V2_NONE;
}

static int name_has_family_prefix(const char *name) {
  size_t i;

  for (i = 0u; i < sizeof k_family_prefixes / sizeof k_family_prefixes[0];
       ++i) {
    size_t length = strlen(k_family_prefixes[i]);
    if (strncmp(name, k_family_prefixes[i], length) == 0) {
      return 1;
    }
  }
  return 0;
}

static int name_looks_like_object(const char *name) {
  return strstr(name, ".so") != NULL;
}

/* Um candidato so' vale se UM MESMO objeto exportar o EGL e todos os pontos
 * de entrada GLES que o port declarou. E' isso que separa o blob unificado da
 * Mesa sem driver -- e e' prova por simbolo, nao por nome. */
static int object_exports_everything(const char *path,
                                     const char *const *symbols,
                                     size_t symbol_count) {
  void *handle;
  size_t i;
  int ok;

  handle = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
  if (handle == NULL) {
    return 0;
  }
  ok = dlsym(handle, "eglGetDisplay") != NULL &&
       dlsym(handle, "eglInitialize") != NULL;
  for (i = 0u; ok && i < symbol_count; ++i) {
    if (symbols[i] != NULL && dlsym(handle, symbols[i]) == NULL) {
      ok = 0;
    }
  }
  dlclose(handle);
  return ok;
}

static int scan_directory(const char *directory,
                          const char *const *required_gles_symbols,
                          size_t required_gles_symbol_count, char *out,
                          size_t out_size, int *seen) {
  char path[NXGL_PROVIDER_RECOVERY_PATH_MAX];
  struct dirent *entry;
  DIR *handle;
  int written;

  if (directory == NULL || directory[0] == '\0') {
    return 0;
  }
  handle = opendir(directory);
  if (handle == NULL) {
    return 0;
  }
  while ((entry = readdir(handle)) != NULL) {
    if (entry->d_name[0] == '.' || !name_has_family_prefix(entry->d_name) ||
        !name_looks_like_object(entry->d_name)) {
      continue;
    }
    written = snprintf(path, sizeof path, "%s/%s", directory, entry->d_name);
    if (written <= 0 || (size_t)written >= sizeof path) {
      continue;
    }
    if (seen != NULL) {
      ++(*seen);
    }
    if (!object_exports_everything(path, required_gles_symbols,
                                   required_gles_symbol_count)) {
      continue;
    }
    written = snprintf(out, out_size, "%s", path);
    if (written > 0 && (size_t)written < out_size) {
      closedir(handle);
      return 1;
    }
  }
  closedir(handle);
  return 0;
}

int nxgl_provider_discover_unified(const char *const *required_gles_symbols,
                                   size_t required_gles_symbol_count,
                                   const char *const *extra_library_dirs,
                                   size_t extra_library_dir_count, char *out,
                                   size_t out_size) {
  size_t i;

  if (out == NULL || out_size == 0u) {
    return 0;
  }
  out[0] = '\0';
  for (i = 0u; i < extra_library_dir_count; ++i) {
    if (extra_library_dirs != NULL &&
        scan_directory(extra_library_dirs[i], required_gles_symbols,
                       required_gles_symbol_count, out, out_size, NULL)) {
      return 1;
    }
  }
  for (i = 0u; i < sizeof k_library_dirs / sizeof k_library_dirs[0]; ++i) {
    if (scan_directory(k_library_dirs[i], required_gles_symbols,
                       required_gles_symbol_count, out, out_size, NULL)) {
      return 1;
    }
  }
  out[0] = '\0';
  return 0;
}

static void receipt_say(nxgl_provider_repair_receipt *receipt,
                        const char *format, ...) {
  va_list args;

  if (receipt == NULL) {
    return;
  }
  va_start(args, format);
  vsnprintf(receipt->text, sizeof receipt->text, format, args);
  va_end(args);
}

int nxgl_provider_repair_if_renderer_broken(
    const nxgl_provider_repair_options *options,
    nxgl_provider_repair_receipt *receipt) {
  nxgl_sdl_provider_probe_options_v2 probe_options;
  nxgl_sdl_provider_probe_receipt_v2 probe_receipt;
  nxgl_sdl_provider_reexec_options_v2 reexec_options;
  nxgl_sdl_provider_reexec_result_v2 reexec_result;
  nxgl_sdl_provider_pair_plan plan;
  char provider[NXGL_PROVIDER_RECOVERY_PATH_MAX];
  int seen = 0;

  if (receipt != NULL) {
    nxgl_provider_repair_receipt_init(receipt);
  }
  if (options == NULL || options->struct_size != sizeof(*options)) {
    return NXGL_PROVIDER_REPAIR_NO_ACTION;
  }

  /* Renderer saudavel: o aparelho nao e' tocado. Este e' o caso da esmagadora
   * maioria, inclusive do Mali-450, e por isso a checagem vem primeiro. */
  if (!nxgl_provider_renderer_is_broken(options->renderer)) {
    receipt_say(receipt, "GL PROVIDER: renderer ok (%s); nada a fazer",
                options->renderer);
    return NXGL_PROVIDER_REPAIR_NO_ACTION;
  }
  /* Uma tentativa por processo: o marcador sobrevive ao exec. */
  if (getenv(NXGL_PROVIDER_RECOVERY_MARKER) != NULL) {
    receipt_say(receipt,
                "GL PROVIDER: renderer ainda vazio apos um reparo; desistindo");
    return NXGL_PROVIDER_REPAIR_NO_ACTION;
  }

  if (!nxgl_provider_discover_unified(
          options->required_gles_symbols, options->required_gles_symbol_count,
          options->extra_library_dirs, options->extra_library_dir_count,
          provider, sizeof provider)) {
    if (receipt != NULL) {
      receipt->outcome = NXGL_PROVIDER_REPAIR_NO_CANDIDATE;
      receipt->candidates_seen = seen;
    }
    receipt_say(receipt,
                "GL PROVIDER: renderer vazio e nenhum objeto unificado com "
                "EGL + GLES do port");
    return NXGL_PROVIDER_REPAIR_NO_CANDIDATE;
  }

  nxgl_sdl_provider_probe_options_v2_init(&probe_options);
  nxgl_sdl_provider_probe_receipt_v2_init(&probe_receipt);
  probe_options.enabled = 1;
  probe_options.mode = NXGL_PROVIDER_PROBE_V2_SYMBOLS_ONLY;
  probe_options.reject_if_already_loaded = 0;
  probe_options.video_torn_down = 0;
  probe_options.video_backend = options->video_backend;
  probe_options.provider = provider;
  probe_options.required_engine_gles_symbols = options->required_gles_symbols;
  probe_options.required_engine_gles_symbol_count =
      options->required_gles_symbol_count;
  (void)nxgl_probe_sdl_provider_v2(&probe_options, &probe_receipt);
  if (receipt != NULL) {
    receipt->candidates_probed = 1;
    receipt->reason = probe_receipt.reason;
    snprintf(receipt->provider_path, sizeof receipt->provider_path, "%s",
             probe_receipt.provider_path[0] != '\0'
                 ? probe_receipt.provider_path
                 : provider);
  }
  if (!probe_receipt.usable) {
    if (receipt != NULL) {
      receipt->outcome = NXGL_PROVIDER_REPAIR_NO_CANDIDATE;
    }
    receipt_say(receipt, "GL PROVIDER: candidato %s reprovado (%s)", provider,
                nxgl_provider_recovery_reason_name_v2(probe_receipt.reason));
    return NXGL_PROVIDER_REPAIR_NO_CANDIDATE;
  }

  plan = nxgl_plan_sdl_provider_pair(
      options->video_backend, options->renderer, probe_receipt.provider_path,
      options->window_opened, options->context_current,
      options->drawable_positive, probe_receipt.exports_egl,
      probe_receipt.exports_engine_gles);
  if (plan != NXGL_SDL_PROVIDER_PAIR_BIND_COHERENT) {
    if (receipt != NULL) {
      receipt->outcome = NXGL_PROVIDER_REPAIR_NO_ACTION;
    }
    receipt_say(receipt, "GL PROVIDER: nao autorizado pelo plano (%d)",
                (int)plan);
    return NXGL_PROVIDER_REPAIR_NO_ACTION;
  }

  /* O teardown e' condicao do contrato: o re-exec e' recusado sem ele. */
  if (options->teardown != NULL) {
    options->teardown();
  }

  /* 0.2.13 (campo dArkOS, ES-launch): a firmware exporta
   * SDL_VIDEO_EGL_DRIVER=libEGL.so no unit do frontend, e "hint herdado manda
   * mais que o reparo" deixava o renderer MORTO sem conserto -- tela preta
   * com som para todo lancamento pelo menu. Aqui, e SOMENTE aqui, o hint
   * acabou de ser DESPROVADO por medicao viva: o contexto que ele produziu
   * neste processo devolveu renderer nulo. Um hint desprovado deixa de
   * mandar. Guardamos os valores, desamarramos e seguimos para o re-exec com
   * o par coerente; se o exec falhar, os valores voltam para o post-mortem.
   * No caminho PRE-CONTEXTO nada muda: sem criterio vivo, o hint da CFW
   * continua soberano. */
  {
    const char *hinted_egl = getenv("SDL_VIDEO_EGL_DRIVER");
    const char *hinted_gl = getenv("SDL_VIDEO_GL_DRIVER");
    char saved_egl[NXGL_PROVIDER_RECOVERY_PATH_MAX] = "";
    char saved_gl[NXGL_PROVIDER_RECOVERY_PATH_MAX] = "";
    int hint_disproven = 0;

    if (hinted_egl != NULL || hinted_gl != NULL) {
      hint_disproven = 1;
      if (hinted_egl != NULL) {
        snprintf(saved_egl, sizeof saved_egl, "%s", hinted_egl);
        unsetenv("SDL_VIDEO_EGL_DRIVER");
      }
      if (hinted_gl != NULL) {
        snprintf(saved_gl, sizeof saved_gl, "%s", hinted_gl);
        unsetenv("SDL_VIDEO_GL_DRIVER");
      }
      receipt_say(receipt,
                  "GL PROVIDER: hint herdado (%s) desprovado por renderer "
                  "morto neste boot; sobrescrevendo pelo par coerente",
                  saved_egl[0] != '\0' ? saved_egl : saved_gl);
    }

    nxgl_sdl_provider_reexec_options_v2_init(&reexec_options);
    nxgl_sdl_provider_reexec_result_v2_init(&reexec_result);
    reexec_options.enabled = 1;
    reexec_options.authorization = plan;
    reexec_options.provider = &probe_receipt;
    reexec_options.video_torn_down = 1;
    reexec_options.argv = options->argv;
    (void)nxgl_reexec_sdl_provider_pair_v2(&reexec_options, &reexec_result);

    /* Chegar aqui significa que o exec falhou; o helper ja' restaurou o que
     * ele proprio mudou. O hint desprovado volta para o post-mortem. */
    if (hint_disproven) {
      if (saved_egl[0] != '\0') {
        setenv("SDL_VIDEO_EGL_DRIVER", saved_egl, 1);
      }
      if (saved_gl[0] != '\0') {
        setenv("SDL_VIDEO_GL_DRIVER", saved_gl, 1);
      }
    }
  }
  if (receipt != NULL) {
    receipt->outcome = NXGL_PROVIDER_REPAIR_FAILED;
    receipt->reason = reexec_result.reason;
  }
  receipt_say(receipt, "GL PROVIDER: re-exec falhou (%s)",
              nxgl_provider_recovery_reason_name_v2(reexec_result.reason));
  return NXGL_PROVIDER_REPAIR_FAILED;
}

/* Um hint de provedor herdado significa que a CFW (ou o usuario) ja' escolheu.
 * Respeitar essa escolha vem antes de qualquer reparo nosso. */
static int inherited_provider_hint(void) {
  const char *egl = getenv("SDL_VIDEO_EGL_DRIVER");
  const char *gl = getenv("SDL_VIDEO_GL_DRIVER");
  return (egl != NULL && egl[0] != '\0') || (gl != NULL && gl[0] != '\0');
}

/* 0.2.12: o criterio vivo e' a TENTATIVA REAL, nunca uma sonda propria.
 *
 * A 0.2.11 tentou provar o candidato com eglGetDisplay+eglInitialize antes do
 * re-exec e reprovou o blob BOM do dArkOS: blobs -gbm devolvem display nulo no
 * default display mesmo em processo limpo (o display deles nasce da plataforma
 * GBM, que so' a SDL monta). Medido no aparelho em 22/08/2026. A unica prova
 * que nao mente e' deixar a SDL tentar com o par aplicado.
 *
 * Entao o contrato vira: o re-exec continua como sempre; se NO PROCESSO
 * RE-EXECUTADO a janela falhar de novo, o port chama este rollback -- o par
 * e' desamarrado no proprio processo, a receita registra, e o port repete a
 * criacao de janela pelo caminho normal da firmware. No aparelho onde o blob
 * e' o certo nada disso roda; no firmware com blob orfao (FF4 3D/ROCKNIX) o
 * erro final volta a ser o REAL da pilha da firmware, nao o do blob morto.
 * Uma unica vez por processo: segunda chamada nao faz nada. */
int nxgl_provider_precontext_rollback(nxgl_provider_repair_receipt *receipt) {
  static int already_rolled_back;
  const char *egl = getenv("SDL_VIDEO_EGL_DRIVER");
  const char *gl = getenv("SDL_VIDEO_GL_DRIVER");

  if (receipt != NULL) {
    nxgl_provider_repair_receipt_init(receipt);
  }
  if (already_rolled_back) {
    receipt_say(receipt, "GL PROVIDER: rollback ja' consumido neste processo");
    return 0;
  }
  if (getenv(NXGL_PROVIDER_RECOVERY_MARKER) == NULL) {
    receipt_say(receipt,
                "GL PROVIDER: sem reparo aplicado; nada para desfazer");
    return 0;
  }
  if ((egl == NULL || egl[0] == '\0') && (gl == NULL || gl[0] == '\0')) {
    receipt_say(receipt,
                "GL PROVIDER: reparo marcado mas sem par no ambiente");
    return 0;
  }
  already_rolled_back = 1;
  unsetenv("SDL_VIDEO_EGL_DRIVER");
  unsetenv("SDL_VIDEO_GL_DRIVER");
  if (receipt != NULL) {
    receipt->outcome = NXGL_PROVIDER_REPAIR_NO_CANDIDATE;
  }
  receipt_say(receipt,
              "GL PROVIDER: par aplicado falhou na tentativa real; "
              "desamarrado -- repetindo pela pilha da firmware");
  return 1;
}

int nxgl_provider_repair_precontext(
    const nxgl_provider_repair_options *options,
    nxgl_open_stage_v2 failed_stage, nxgl_open_reason_v2 final_reason,
    nxgl_provider_repair_receipt *receipt) {
  nxgl_sdl_provider_probe_options_v2 probe_options;
  nxgl_sdl_provider_probe_receipt_v2 probe_receipt;
  nxgl_sdl_provider_reexec_options_v2 reexec_options;
  nxgl_sdl_provider_reexec_result_v2 reexec_result;
  nxgl_sdl_precontext_recovery_v2 recovery;
  nxgl_sdl_provider_pair_plan plan;
  char provider[NXGL_PROVIDER_RECOVERY_PATH_MAX];

  if (receipt != NULL) {
    nxgl_provider_repair_receipt_init(receipt);
  }
  if (options == NULL || options->struct_size != sizeof(*options)) {
    return NXGL_PROVIDER_REPAIR_NO_ACTION;
  }
  if (getenv(NXGL_PROVIDER_RECOVERY_MARKER) != NULL) {
    receipt_say(receipt,
                "GL PROVIDER: pre-contexto ainda falhando apos um reparo; "
                "desistindo");
    return NXGL_PROVIDER_REPAIR_NO_ACTION;
  }
  if (!nxgl_provider_discover_unified(
          options->required_gles_symbols, options->required_gles_symbol_count,
          options->extra_library_dirs, options->extra_library_dir_count,
          provider, sizeof provider)) {
    if (receipt != NULL) {
      receipt->outcome = NXGL_PROVIDER_REPAIR_NO_CANDIDATE;
    }
    receipt_say(receipt,
                "GL PROVIDER: janela/contexto falharam e nenhum objeto "
                "unificado foi encontrado");
    return NXGL_PROVIDER_REPAIR_NO_CANDIDATE;
  }

  nxgl_sdl_provider_probe_options_v2_init(&probe_options);
  nxgl_sdl_provider_probe_receipt_v2_init(&probe_receipt);
  probe_options.enabled = 1;
  probe_options.mode = NXGL_PROVIDER_PROBE_V2_SYMBOLS_ONLY;
  probe_options.video_backend = options->video_backend;
  probe_options.provider = provider;
  probe_options.required_engine_gles_symbols = options->required_gles_symbols;
  probe_options.required_engine_gles_symbol_count =
      options->required_gles_symbol_count;
  (void)nxgl_probe_sdl_provider_v2(&probe_options, &probe_receipt);
  if (receipt != NULL) {
    receipt->candidates_probed = 1;
    receipt->reason = probe_receipt.reason;
    snprintf(receipt->provider_path, sizeof receipt->provider_path, "%s",
             probe_receipt.provider_path[0] != '\0'
                 ? probe_receipt.provider_path
                 : provider);
  }
  if (!probe_receipt.usable) {
    if (receipt != NULL) {
      receipt->outcome = NXGL_PROVIDER_REPAIR_NO_CANDIDATE;
    }
    receipt_say(receipt, "GL PROVIDER: candidato %s reprovado (%s)", provider,
                nxgl_provider_recovery_reason_name_v2(probe_receipt.reason));
    return NXGL_PROVIDER_REPAIR_NO_CANDIDATE;
  }

  nxgl_sdl_precontext_recovery_v2_init(&recovery);
  recovery.video_backend = options->video_backend;
  recovery.provider_name = probe_receipt.provider_path;
  recovery.failed_stage = failed_stage;
  recovery.final_reason = final_reason;
  recovery.attempts_exhausted = 1;
  recovery.inherited_provider_hint = inherited_provider_hint();
  recovery.same_object = 1;
  recovery.exports_egl = probe_receipt.exports_egl;
  recovery.exports_engine_gles = probe_receipt.exports_engine_gles;
  plan = nxgl_plan_sdl_precontext_recovery_v2(&recovery);
  if (plan != NXGL_SDL_PROVIDER_PAIR_BIND_COHERENT) {
    if (receipt != NULL) {
      receipt->outcome = NXGL_PROVIDER_REPAIR_NO_ACTION;
    }
    receipt_say(receipt,
                "GL PROVIDER: pre-contexto nao autorizado pelo plano (%d%s)",
                (int)plan,
                recovery.inherited_provider_hint ? ", hint herdado" : "");
    return NXGL_PROVIDER_REPAIR_NO_ACTION;
  }

  if (options->teardown != NULL) {
    options->teardown();
  }
  nxgl_sdl_provider_reexec_options_v2_init(&reexec_options);
  nxgl_sdl_provider_reexec_result_v2_init(&reexec_result);
  reexec_options.enabled = 1;
  reexec_options.authorization = plan;
  reexec_options.provider = &probe_receipt;
  reexec_options.video_torn_down = 1;
  reexec_options.argv = options->argv;
  (void)nxgl_reexec_sdl_provider_pair_v2(&reexec_options, &reexec_result);

  if (receipt != NULL) {
    receipt->outcome = NXGL_PROVIDER_REPAIR_FAILED;
    receipt->reason = reexec_result.reason;
  }
  receipt_say(receipt, "GL PROVIDER: re-exec pre-contexto falhou (%s)",
              nxgl_provider_recovery_reason_name_v2(reexec_result.reason));
  return NXGL_PROVIDER_REPAIR_FAILED;
}
