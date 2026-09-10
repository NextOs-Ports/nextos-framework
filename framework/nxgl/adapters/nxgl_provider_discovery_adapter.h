/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXGL_PROVIDER_DISCOVERY_ADAPTER_H
#define NXGL_PROVIDER_DISCOVERY_ADAPTER_H

/* Reparo do par EGL/GLES por CRITERIO OBSERVADO, nao por nome de arquivo.
 *
 * O sintoma, medido em campo em varios clones de dArkOS e reproduzido num
 * R36S com Mali-Bifrost G31: os SONAMEs graficos estao CRUZADOS.
 *
 *   libGLESv1_CM.so.1 -> uma Mesa de 198 KB, SEM driver atras
 *   libGLESv1_CM.so   -> o blob Mali real, 40 MB
 *   libmali.so        -> o mesmo blob de 40 MB
 *
 * O binario se liga por SONAME, entao ganha um contexto que aceita toda
 * chamada e nao desenha nada. Audio, input e o laco seguem vivos, o processo
 * sai 0, e o painel fica preto. Nao ha erro em lugar nenhum.
 *
 * A UNICA condicao segura e observavel e' a string de renderer VAZIA medida no
 * contexto real. Uma Mesa saudavel (Panfrost no ROCKNIX) reporta um renderer
 * de verdade e nao e' tocada. Escolher provedor por ordem de nome de arquivo
 * -- que e' o que se faz sem este modulo -- e' heuristica: acerta no aparelho
 * onde foi testada e erra no proximo.
 *
 * O framework ja' tinha as duas metades dificeis e nenhuma delas era usada:
 * `nxgl_plan_sdl_provider_pair()` (autorizacao pura) e
 * `nxgl_probe_sdl_provider_v2()` / `nxgl_reexec_sdl_provider_pair_v2()`
 * (prova e re-exec). O que faltava, e o que este adapter acrescenta, e' a
 * DESCOBERTA de candidatos -- que o nucleo recusa fazer de proposito, porque
 * varrer diretorio de firmware nao e' comportamento de contrato.
 *
 * A descoberta e' por FAMILIA DE ARQUIVO, nunca por nome de aparelho ou de
 * CFW: um objeto so' vira candidato depois de provar, por dlsym, que exporta
 * EGL e os pontos de entrada GLES que o port declarou.
 *
 * USO, uma chamada depois que o contexto reportou o renderer:
 *
 *   nxgl_provider_repair_options opts;
 *   nxgl_provider_repair_options_init(&opts);
 *   opts.renderer = glGetString(GL_RENDERER);
 *   opts.video_backend = SDL_GetCurrentVideoDriver();
 *   opts.window_opened = 1;
 *   opts.context_current = 1;
 *   opts.drawable_positive = (w > 0 && h > 0);
 *   opts.required_gles_symbols = k_symbols;   -- do proprio port
 *   opts.required_gles_symbol_count = ...;
 *   opts.teardown = meu_teardown;             -- fecha contexto/janela/SDL
 *   opts.argv = argv;
 *   nxgl_provider_repair_if_renderer_broken(&opts, &receipt);
 *
 * Em caso de reparo a funcao NAO retorna: o processo re-executa uma vez, com
 * SDL_VIDEO_EGL_DRIVER e SDL_VIDEO_GL_DRIVER apontando para o MESMO objeto
 * validado. Um marcador de ambiente impede a segunda tentativa.
 */

#include "nxgl.h"
#include "nxgl_provider_recovery.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXGL_PROVIDER_REPAIR_RECEIPT_MAX 320u

typedef enum nxgl_provider_repair_outcome {
  /* O renderer estava saudavel, ou nao ha' o que fazer. Nada foi tocado. */
  NXGL_PROVIDER_REPAIR_NO_ACTION = 0,
  /* Renderer vazio, mas nenhum candidato passou na prova. */
  NXGL_PROVIDER_REPAIR_NO_CANDIDATE = 1,
  /* Autorizado e re-executado -- nao se observa este valor no processo que
   * chamou, porque ele nao volta. Existe para o gate. */
  NXGL_PROVIDER_REPAIR_REEXECUTED = 2,
  /* Autorizado, mas o re-exec falhou. O ambiente foi restaurado. */
  NXGL_PROVIDER_REPAIR_FAILED = 3
} nxgl_provider_repair_outcome;

typedef struct nxgl_provider_repair_options {
  uint32_t api_version;
  size_t struct_size;
  const char *renderer;
  const char *video_backend;
  int window_opened;
  int context_current;
  int drawable_positive;
  const char *const *required_gles_symbols;
  size_t required_gles_symbol_count;
  /* Diretorios extras a varrer antes dos padroes. Pode ser NULL. */
  const char *const *extra_library_dirs;
  size_t extra_library_dir_count;
  /* O adapter e' dono do teardown: o re-exec e' recusado sem essa atestacao. */
  void (*teardown)(void);
  char *const *argv;
} nxgl_provider_repair_options;

typedef struct nxgl_provider_repair_receipt {
  uint32_t api_version;
  size_t struct_size;
  nxgl_provider_repair_outcome outcome;
  nxgl_provider_recovery_reason_v2 reason;
  int candidates_seen;
  int candidates_probed;
  char provider_path[NXGL_PROVIDER_RECOVERY_PATH_MAX];
  char text[NXGL_PROVIDER_REPAIR_RECEIPT_MAX];
} nxgl_provider_repair_receipt;

void nxgl_provider_repair_options_init(nxgl_provider_repair_options *options);
void nxgl_provider_repair_receipt_init(nxgl_provider_repair_receipt *receipt);

/* Puro: 1 quando a string de renderer indica um provedor morto. */
int nxgl_provider_renderer_is_broken(const char *renderer);

/* Varre as familias de blob unificado conhecidas e devolve, em `out`, o
 * primeiro objeto que exporta EGL e todos os simbolos GLES pedidos. Nao abre
 * display nem toca em ambiente. Devolve 1 se achou. */
int nxgl_provider_discover_unified(const char *const *required_gles_symbols,
                                   size_t required_gles_symbol_count,
                                   const char *const *extra_library_dirs,
                                   size_t extra_library_dir_count, char *out,
                                   size_t out_size);

/* Reparo PRE-CONTEXTO. O sintoma aqui e' outro: a SDL sobe o video, mas
 * `SDL_CreateWindow`/`SDL_GL_CreateContext` falham porque os SONAMEs de EGL e
 * GLES que o loader resolveu apontam para objetos DIFERENTES -- a janela nem
 * chega a existir, entao nao ha renderer para medir e o reparo reativo nunca
 * roda. Medido no dArkOS: sem o hint de EGL que a CFW exporta no unit do
 * frontend, um lancamento por outro caminho morre em "Can't load EGL/GL
 * library on window creation".
 *
 * Nao ha criterio vivo possivel aqui, entao a autorizacao e' mais estreita: o
 * framework so' autoriza quando as tentativas se esgotaram, quando o ambiente
 * NAO trouxe um hint de provedor herdado (respeitar a escolha da CFW vem
 * antes) e quando um unico objeto exporta EGL e os GLES pedidos.
 *
 * `failed_stage`/`final_reason` devem descrever a falha real observada. */
/* 0.2.12: desamarra o par aplicado quando a TENTATIVA REAL falhou no processo
 * re-executado (criterio vivo = a propria SDL; sonda EGL propria reprova blob
 * -gbm bom, medido no aparelho). Retorna 1 quando desamarrou e o chamador deve
 * repetir a criacao de janela pelo caminho normal; 0 quando nao ha nada a
 * desfazer ou o rollback ja' foi consumido neste processo. */
int nxgl_provider_precontext_rollback(nxgl_provider_repair_receipt *receipt);

int nxgl_provider_repair_precontext(
    const nxgl_provider_repair_options *options,
    nxgl_open_stage_v2 failed_stage, nxgl_open_reason_v2 final_reason,
    nxgl_provider_repair_receipt *receipt);

/* Orquestra tudo. Ver o cabecalho deste arquivo. */
int nxgl_provider_repair_if_renderer_broken(
    const nxgl_provider_repair_options *options,
    nxgl_provider_repair_receipt *receipt);

#ifdef __cplusplus
}
#endif

#endif /* NXGL_PROVIDER_DISCOVERY_ADAPTER_H */
