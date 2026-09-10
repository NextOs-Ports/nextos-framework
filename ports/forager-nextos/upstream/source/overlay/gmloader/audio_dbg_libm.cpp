/* [NextOS/forager] Sonda de libm do caminho de audio — diagnostico do MUDO.
 *
 * O mixer do runner (OpenAL estatico no libyoyo, guest softfp) atravessa para o host
 * hardfp SOMENTE via thunks de libm: lrintf/lrint (float->s16), sinf/cosf (panning),
 * expf/pow (curva de ganho), sqrtf. Esta tabela entra ANTES da symtable_libc em
 * so_dynamic_libraries, entao estes wrappers vencem a resolucao e LOGAM amostras de
 * entrada/saida quando GMLOADER_AUDIO_DEBUG esta setado. Sem a env var, e so um
 * branch morto por chamada (regra #26). Se a entrada chegar como lixo (~1e-39) o
 * bug e de ABI no thunk; se entrada e saida forem sadias, o culpado e o guest.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "platform.h"
#include "so_util.h"

static int adbg(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("GMLOADER_AUDIO_DEBUG");
        v = (e && *e) ? 1 : 0;
    }
    return v;
}

#define SAMPLED_LOG(fmt, ...)                       \
    do {                                            \
        static int n = 0;                           \
        if (adbg() && (n++ % 5000) == 0)            \
            printf("[libm] " fmt "\n", __VA_ARGS__); \
    } while (0)

ABI_ATTR static long lrintf_dbg(float x)
{
    long r = lrintf(x);
    SAMPLED_LOG("lrintf(%g) -> %ld", (double)x, r);
    return r;
}

ABI_ATTR static long lrint_dbg(double x)
{
    long r = lrint(x);
    SAMPLED_LOG("lrint(%g) -> %ld", x, r);
    return r;
}

ABI_ATTR static long long llrint_dbg(double x)
{
    long long r = llrint(x);
    SAMPLED_LOG("llrint(%g) -> %lld", x, r);
    return r;
}

ABI_ATTR static float expf_dbg(float x)
{
    float r = expf(x);
    SAMPLED_LOG("expf(%g) -> %g", (double)x, (double)r);
    return r;
}

ABI_ATTR static double pow_dbg(double a, double b)
{
    double r = pow(a, b);
    SAMPLED_LOG("pow(%g, %g) -> %g", a, b, r);
    /* ganho minusculo: logar o chamador (lr) para mapear no libyoyo (addr - base) */
    if (adbg() && r < 0.1 && r > 0.0 && a == 2.0) {
        static int m = 0;
        if ((m++ % 50) == 0)
            printf("[libm] pow(2, %g) -> %g LR=%p\n", b, r, __builtin_return_address(0));
    }
    return r;
}

ABI_ATTR static float sinf_dbg(float x)
{
    float r = sinf(x);
    SAMPLED_LOG("sinf(%g) -> %g", (double)x, (double)r);
    return r;
}

ABI_ATTR static float cosf_dbg(float x)
{
    float r = cosf(x);
    SAMPLED_LOG("cosf(%g) -> %g", (double)x, (double)r);
    return r;
}

ABI_ATTR static float sqrtf_dbg(float x)
{
    float r = sqrtf(x);
    SAMPLED_LOG("sqrtf(%g) -> %g", (double)x, (double)r);
    return r;
}

DynLibFunction symtable_audiodbg[] = {
    {"lrintf", (uintptr_t)&lrintf_dbg},
    {"lrint", (uintptr_t)&lrint_dbg},
    {"llrint", (uintptr_t)&llrint_dbg},
    {"expf", (uintptr_t)&expf_dbg},
    {"pow", (uintptr_t)&pow_dbg},
    {"sinf", (uintptr_t)&sinf_dbg},
    {"cosf", (uintptr_t)&cosf_dbg},
    {"sqrtf", (uintptr_t)&sqrtf_dbg},
    {NULL, 0},
};
