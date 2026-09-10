/*
 * Optional ARMv7 softfp -> hardfp libm provider, based on the approved KOTOR
 * boundary. It is never enabled implicitly by nxloader.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "nxloader_softfp.h"

#if defined(__arm__)

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NX_SF1D(name) \
  NXLOADER_ARM_SOFTFP static double nx_sf_##name(double x) { return name(x); }
#define NX_SF1F(name) \
  NXLOADER_ARM_SOFTFP static float nx_sf_##name(float x) { return name(x); }
#define NX_SF2D(name)                                                        \
  NXLOADER_ARM_SOFTFP static double nx_sf_##name(double x, double y) {       \
    return name(x, y);                                                       \
  }
#define NX_SF2F(name)                                                        \
  NXLOADER_ARM_SOFTFP static float nx_sf_##name(float x, float y) {          \
    return name(x, y);                                                       \
  }

NX_SF1D(acos)
NX_SF1D(asin)
NX_SF1D(atan)
NX_SF1D(cos)
NX_SF1D(sin)
NX_SF1D(tan)
NX_SF1D(cosh)
NX_SF1D(sinh)
NX_SF1D(tanh)
NX_SF1D(exp)
NX_SF1D(exp2)
NX_SF1D(log)
NX_SF1D(log10)
NX_SF1D(sqrt)
NX_SF1D(ceil)
NX_SF1D(floor)
NX_SF1D(round)
NX_SF1D(trunc)
NX_SF1D(rint)
NX_SF1D(fabs)
NX_SF1F(acosf)
NX_SF1F(asinf)
NX_SF1F(atanf)
NX_SF1F(cosf)
NX_SF1F(sinf)
NX_SF1F(tanf)
NX_SF1F(expf)
NX_SF1F(exp2f)
NX_SF1F(logf)
NX_SF1F(log2f)
NX_SF1F(log10f)
NX_SF1F(sqrtf)
NX_SF1F(cbrtf)
NX_SF1F(fabsf)
NX_SF1F(ceilf)
NX_SF1F(floorf)
NX_SF1F(roundf)
NX_SF1F(truncf)
NX_SF1F(rintf)
NX_SF1F(coshf)
NX_SF1F(sinhf)
NX_SF1F(tanhf)
NX_SF2D(atan2)
NX_SF2D(fmod)
NX_SF2D(pow)
NX_SF2D(remainder)
NX_SF2D(hypot)
NX_SF2D(fmin)
NX_SF2D(fmax)
NX_SF2D(copysign)
NX_SF2F(atan2f)
NX_SF2F(fmodf)
NX_SF2F(powf)
NX_SF2F(hypotf)
NX_SF2F(fminf)
NX_SF2F(fmaxf)
NX_SF2F(copysignf)

NXLOADER_ARM_SOFTFP static double nx_sf_modf(double x, double *integer) {
  return modf(x, integer);
}
NXLOADER_ARM_SOFTFP static float nx_sf_modff(float x, float *integer) {
  return modff(x, integer);
}
NXLOADER_ARM_SOFTFP static double nx_sf_frexp(double x, int *exponent) {
  return frexp(x, exponent);
}
NXLOADER_ARM_SOFTFP static float nx_sf_frexpf(float x, int *exponent) {
  return frexpf(x, exponent);
}
NXLOADER_ARM_SOFTFP static double nx_sf_ldexp(double x, int exponent) {
  return ldexp(x, exponent);
}
NXLOADER_ARM_SOFTFP static float nx_sf_ldexpf(float x, int exponent) {
  return ldexpf(x, exponent);
}
NXLOADER_ARM_SOFTFP static double nx_sf_strtod(const char *text, char **end) {
  return strtod(text, end);
}
NXLOADER_ARM_SOFTFP static float nx_sf_strtof(const char *text, char **end) {
  return strtof(text, end);
}
NXLOADER_ARM_SOFTFP static void nx_sf_sincosf(float x, float *sine,
                                              float *cosine) {
  *sine = sinf(x);
  *cosine = cosf(x);
}
NXLOADER_ARM_SOFTFP static int nx_sf___isfinitef(float x) {
  return isfinite(x);
}
NXLOADER_ARM_SOFTFP static int nx_sf___isnanf(float x) {
  return isnan(x);
}

#define NX_SF_ADD(function_name)                                            \
  do {                                                                      \
    __typeof__(&nx_sf_##function_name) function_pointer =                   \
        &nx_sf_##function_name;                                             \
    uintptr_t function_address = 0;                                         \
    if (sizeof(function_pointer) != sizeof(function_address)) {             \
      free(symbols);                                                        \
      return NXLOADER_EUNSUPPORTED;                                         \
    }                                                                       \
    if (count >= NX_SOFTFP_CAPACITY) {                                      \
      free(symbols);                                                        \
      return NXLOADER_EOVERFLOW;                                            \
    }                                                                       \
    memcpy(&function_address, &function_pointer, sizeof(function_address)); \
    symbols[count].name = #function_name;                                   \
    symbols[count].address = function_address;                              \
    count++;                                                                \
  } while (0)

nxloader_result nxloader_softfp_add_libm(nxloader_registry *registry,
                                        const char *provider_name,
                                        int priority,
                                        nxloader_registry_report *report) {
  nxloader_symbol *symbols;
  size_t count = 0;
  nxloader_provider provider;
  nxloader_result result;
  enum { NX_SOFTFP_CAPACITY = 72 };
  if (!registry || !provider_name || !*provider_name)
    return NXLOADER_EINVAL;
  symbols = (nxloader_symbol *)calloc(NX_SOFTFP_CAPACITY, sizeof(*symbols));
  if (!symbols)
    return NXLOADER_ENOMEM;
  NX_SF_ADD(acos); NX_SF_ADD(asin); NX_SF_ADD(atan); NX_SF_ADD(cos);
  NX_SF_ADD(sin); NX_SF_ADD(tan); NX_SF_ADD(cosh); NX_SF_ADD(sinh);
  NX_SF_ADD(tanh); NX_SF_ADD(exp); NX_SF_ADD(exp2); NX_SF_ADD(log);
  NX_SF_ADD(log10); NX_SF_ADD(sqrt); NX_SF_ADD(ceil); NX_SF_ADD(floor);
  NX_SF_ADD(round); NX_SF_ADD(trunc); NX_SF_ADD(rint); NX_SF_ADD(fabs);
  NX_SF_ADD(acosf); NX_SF_ADD(asinf); NX_SF_ADD(atanf); NX_SF_ADD(cosf);
  NX_SF_ADD(sinf); NX_SF_ADD(tanf); NX_SF_ADD(expf); NX_SF_ADD(exp2f);
  NX_SF_ADD(logf); NX_SF_ADD(log2f); NX_SF_ADD(log10f); NX_SF_ADD(sqrtf);
  NX_SF_ADD(cbrtf); NX_SF_ADD(fabsf); NX_SF_ADD(ceilf); NX_SF_ADD(floorf);
  NX_SF_ADD(roundf); NX_SF_ADD(truncf); NX_SF_ADD(rintf); NX_SF_ADD(coshf);
  NX_SF_ADD(sinhf); NX_SF_ADD(tanhf); NX_SF_ADD(atan2); NX_SF_ADD(fmod);
  NX_SF_ADD(pow); NX_SF_ADD(remainder); NX_SF_ADD(hypot); NX_SF_ADD(fmin);
  NX_SF_ADD(fmax); NX_SF_ADD(copysign); NX_SF_ADD(atan2f); NX_SF_ADD(fmodf);
  NX_SF_ADD(powf); NX_SF_ADD(hypotf); NX_SF_ADD(fminf); NX_SF_ADD(fmaxf);
  NX_SF_ADD(copysignf); NX_SF_ADD(modf); NX_SF_ADD(modff); NX_SF_ADD(frexp);
  NX_SF_ADD(frexpf); NX_SF_ADD(ldexp); NX_SF_ADD(ldexpf);
  NX_SF_ADD(strtod); NX_SF_ADD(strtof); NX_SF_ADD(sincosf);
  NX_SF_ADD(__isfinitef); NX_SF_ADD(__isnanf);
  memset(&provider, 0, sizeof(provider));
  provider.struct_size = sizeof(provider);
  provider.name = provider_name;
  provider.symbols = symbols;
  provider.symbol_count = count;
  provider.priority = priority;
  result = nxloader_registry_add_provider(registry, &provider, report);
  free(symbols);
  return result;
}

#else

nxloader_result nxloader_softfp_add_libm(nxloader_registry *registry,
                                        const char *provider_name,
                                        int priority,
                                        nxloader_registry_report *report) {
  (void)registry;
  (void)provider_name;
  (void)priority;
  (void)report;
  return NXLOADER_EARCH;
}

#endif
