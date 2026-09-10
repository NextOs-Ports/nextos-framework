/* Emissor do receipt nx-graphics-evidence/1 do contrato V3-GRAPHICS-02.
 *
 * O contrato declarado aqui espelha o bloco `graphics` do nxproject: GLES,
 * profile ES, mínimo 2.0, dialeto ESSL100, drawable em até 8000 ms. O adapter
 * vendorizado MEDE o contexto vivo (glGetString/EGL/SDL), roda o shader probe
 * real e escreve o documento JSON. A medição roda uma única vez, no primeiro
 * present com drawable válido; a procedência vem do ambiente que o launcher
 * exportou e é REGISTRADA, nunca decide nada.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "nxgl_graphics_contract_adapter.h"

extern void nx_log(const char *fmt, ...);
extern void *SDL_GL_GetProcAddress(const char *name);

static void st_evidence_bridge_env(const char *dst, const char *src)
{
    const char *v = getenv(dst);
    if (v && v[0])
        return;
    v = getenv(src);
    if (v && v[0])
        setenv(dst, v, 0);
}

void st_graphics_evidence_emit(void)
{
    static int done;
    nxgl_graphics_contract contract;
    nxgl_graphics_evidence evidence;
    static char receipt[8192];
    nxgl_graphics_reason verdict;
    const char *gamedir;
    char path[4096];
    char tmp[4104];
    FILE *out;

    if (done)
        return;
    done = 1;

    /* O launcher 0.6.32 exporta a identidade pela família HEALTH; o adapter
     * lê os nomes NX_*. Ponte registrada, sem sobrescrever valor explícito. */
    st_evidence_bridge_env("NX_GENERATION", "NXBOOTSTRAP_HEALTH_GENERATION");
    st_evidence_bridge_env("NX_PORT_ID", "NXBOOTSTRAP_HEALTH_PORT_ID");

    /* O GL real chega pelo SDL (dlopen RTLD_LOCAL): o RTLD_DEFAULT do
     * resolver padrão não o enxerga. SDL_GL_GetProcAddress é a autoridade. */
    nxgl_graphics_contract_adapter_set_resolver(SDL_GL_GetProcAddress);

    if (nxgl_graphics_contract_default(&contract) != 0)
        return;
    contract.api = NXGL_GRAPHICS_API_GLES;
    contract.profile = NXGL_GRAPHICS_PROFILE_ES;
    contract.version_major = 2;
    contract.version_minor = 0;
    contract.version_policy = NXGL_GRAPHICS_POLICY_MINIMUM;
    contract.shader_dialect = NXGL_SHADER_DIALECT_ESSL100;
    contract.drawable_ready_timeout_ms = 8000;

    verdict = nxgl_graphics_contract_adapter_evidence(
        &contract, &evidence, receipt, sizeof(receipt));

    nx_log("GRAPHICS-EVIDENCE-DIAG verdict=%d renderer=%s gl=%s glsl=%s",
           (int)verdict, evidence.renderer, evidence.gl_version_str,
           evidence.glsl_version);
    /* Linha CANONICA que o nxrelease liga a um graphics_proof (generation +
     * build_id reais). SEMPRE no log -- e' a prova fisica que a promocao
     * exige, jamais escrita a mao. */
    {
        static char line[2048];
        if (nxgl_graphics_contract_evidence_receipt(&contract, &evidence,
                                                    line, sizeof(line)) > 0)
            nx_log("%s", line);
    }
    if (receipt[0])
        nx_log("VIDEO: %s", receipt);

    gamedir = getenv("NXBOOTSTRAP_LOGICAL_GAMEDIR");
    if (!gamedir || !gamedir[0])
        gamedir = ".";
    (void)snprintf(path, sizeof(path), "%s/nx-graphics-evidence.json", gamedir);
    (void)snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    out = fopen(tmp, "w");
    if (out) {
        fputs(receipt, out);
        fputc('\n', out);
        fclose(out);
        if (rename(tmp, path) != 0)
            unlink(tmp);
    }
}
