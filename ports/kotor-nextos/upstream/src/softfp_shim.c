/* softfp_shim.c -- float-ABI bridge.
 *
 * libKOTOR.so / libandroid_port.so are armeabi-v7a SOFTFP (base AAPCS):
 * float/double args pass in INTEGER registers (r0:r1..). The device glibc
 * libm / libGLESv2 are HARDFP (VFP: d0..d7). Calling them directly from the
 * game's softfp code puts the floats in the wrong registers. Each boundary
 * function that takes a float/double BY VALUE gets a pcs("aapcs") wrapper that
 * receives args the way the game sends them and re-emits the hardfp call (GCC
 * translates the registers).
 *
 * Only libm and the GLES2 float-by-value entry points need this. The GL *v
 * (vector/pointer) variants take a pointer, so they resolve straight to native.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "softfp_shim.h"

/* ---- libm (double/float by value) ---------------------------------------- */
#define W1D(n)  SF double sf_##n(double x){ return n(x); }
#define W1F(n)  SF float  sf_##n(float x){ return n(x); }
#define W2D(n)  SF double sf_##n(double a,double b){ return n(a,b); }
#define W2F(n)  SF float  sf_##n(float a,float b){ return n(a,b); }

W1D(acos) W1D(asin) W1D(atan) W1D(cos) W1D(sin) W1D(tan)
W1D(cosh) W1D(sinh) W1D(tanh)
W1D(exp) W1D(exp2) W1D(log) W1D(log10) W1D(sqrt)
W1D(ceil) W1D(floor) W1D(round) W1D(trunc) W1D(rint) W1D(fabs)
W1F(acosf) W1F(asinf) W1F(atanf) W1F(cosf) W1F(sinf) W1F(tanf)
W1F(expf) W1F(logf) W1F(log10f) W1F(log2f) W1F(exp2f) W1F(sqrtf) W1F(cbrtf) W1F(fabsf)
W1F(ceilf) W1F(floorf) W1F(roundf) W1F(truncf) W1F(rintf) W1F(sinhf) W1F(coshf) W1F(tanhf)
W2D(atan2) W2D(fmod) W2D(pow) W2D(remainder) W2D(hypot) W2D(fmin) W2D(fmax) W2D(copysign)
W2F(atan2f) W2F(fmodf) W2F(powf) W2F(hypotf) W2F(fminf) W2F(fmaxf) W2F(copysignf)

SF double sf_modf(double x,double *iptr){ return modf(x,iptr); }
SF float  sf_modff(float x,float *iptr){ return modff(x,iptr); }
SF double sf_frexp(double x,int *e){ return frexp(x,e); }
SF float  sf_frexpf(float x,int *e){ return frexpf(x,e); }
SF double sf_ldexp(double x,int e){ return ldexp(x,e); }
SF float  sf_ldexpf(float x,int e){ return ldexpf(x,e); }
SF double sf_strtod(const char *s,char **end){ return strtod(s,end); }
SF float  sf_strtof(const char *s,char **end){ return strtof(s,end); }
SF int    sf_isfinitef(float x){ return isfinite(x); }
SF int    sf_isnanf(float x){ return isnan(x); }
SF void   sf_sincosf(float x, float *s, float *c){ *s = sinf(x); *c = cosf(x); }
SF void   sf_sincos(double x, double *s, double *c){ *s = sin(x); *c = cos(x); }

struct sfent { const char *nm; void *fn; };
static const struct sfent SFTAB[] = {
  {"acos",sf_acos},{"asin",sf_asin},{"atan",sf_atan},{"cos",sf_cos},{"sin",sf_sin},{"tan",sf_tan},
  {"cosh",sf_cosh},{"sinh",sf_sinh},{"tanh",sf_tanh},
  {"exp",sf_exp},{"exp2",sf_exp2},{"log",sf_log},{"log10",sf_log10},{"sqrt",sf_sqrt},
  {"ceil",sf_ceil},{"floor",sf_floor},{"round",sf_round},{"trunc",sf_trunc},{"rint",sf_rint},{"fabs",sf_fabs},
  {"acosf",sf_acosf},{"asinf",sf_asinf},{"atanf",sf_atanf},{"cosf",sf_cosf},{"sinf",sf_sinf},{"tanf",sf_tanf},
  {"expf",sf_expf},{"logf",sf_logf},{"log10f",sf_log10f},{"log2f",sf_log2f},{"exp2f",sf_exp2f},
  {"sqrtf",sf_sqrtf},{"cbrtf",sf_cbrtf},{"fabsf",sf_fabsf},{"rintf",sf_rintf},
  {"sinhf",sf_sinhf},{"coshf",sf_coshf},{"tanhf",sf_tanhf},
  {"ceilf",sf_ceilf},{"floorf",sf_floorf},{"roundf",sf_roundf},{"truncf",sf_truncf},
  {"atan2",sf_atan2},{"fmod",sf_fmod},{"pow",sf_pow},{"remainder",sf_remainder},
  {"hypot",sf_hypot},{"fmin",sf_fmin},{"fmax",sf_fmax},{"copysign",sf_copysign},
  {"atan2f",sf_atan2f},{"fmodf",sf_fmodf},{"powf",sf_powf},
  {"hypotf",sf_hypotf},{"fminf",sf_fminf},{"fmaxf",sf_fmaxf},{"copysignf",sf_copysignf},
  {"modf",sf_modf},{"modff",sf_modff},{"frexp",sf_frexp},{"frexpf",sf_frexpf},
  {"ldexp",sf_ldexp},{"ldexpf",sf_ldexpf},{"strtod",sf_strtod},{"strtof",sf_strtof},
  {"sincosf",sf_sincosf},{"sincos",sf_sincos},
  {"__isfinitef",sf_isfinitef},{"__isnanf",sf_isnanf},
};

void *softfp_resolve(const char *nm){
  if(!nm) return 0;
  for(unsigned i=0;i<sizeof(SFTAB)/sizeof(SFTAB[0]);i++)
    if(!strcmp(nm,SFTAB[i].nm)) return SFTAB[i].fn;
  return 0;
}

int softfp_table_count(void){ return (int)(sizeof(SFTAB)/sizeof(SFTAB[0])); }
int softfp_fill_table(DynLibFunction *dst){
  int n = softfp_table_count();
  for(int i=0;i<n;i++){ dst[i].symbol = (char*)SFTAB[i].nm; dst[i].func = (uintptr_t)SFTAB[i].fn; }
  return n;
}

/* ---- GLES2 float-by-value entry points -----------------------------------
 * The engine (softfp) calls these with floats in integer registers; the
 * wrappers forward to the system libGLESv2 (hardfp). */
#include <GLES2/gl2.h>

SF void sf_glClearColor(float r,float g,float b,float a){ glClearColor(r,g,b,a); }
SF void sf_glClearDepthf(float d){ glClearDepthf(d); }
SF void sf_glDepthRangef(float n,float f){ glDepthRangef(n,f); }
SF void sf_glBlendColor(float r,float g,float b,float a){ glBlendColor(r,g,b,a); }
SF void sf_glLineWidth(float w){ glLineWidth(w); }
SF void sf_glPolygonOffset(float factor,float units){ glPolygonOffset(factor,units); }
SF void sf_glSampleCoverage(float value,unsigned char invert){ glSampleCoverage(value,invert); }
SF void sf_glTexParameterf(unsigned t,unsigned p,float v){ glTexParameterf(t,p,v); }
SF void sf_glUniform1f(int l,float a){ glUniform1f(l,a); }
SF void sf_glUniform2f(int l,float a,float b){ glUniform2f(l,a,b); }
SF void sf_glUniform3f(int l,float a,float b,float c){ glUniform3f(l,a,b,c); }
SF void sf_glUniform4f(int l,float a,float b,float c,float d){ glUniform4f(l,a,b,c,d); }
SF void sf_glVertexAttrib1f(unsigned i,float a){ glVertexAttrib1f(i,a); }
SF void sf_glVertexAttrib2f(unsigned i,float a,float b){ glVertexAttrib2f(i,a,b); }
SF void sf_glVertexAttrib3f(unsigned i,float a,float b,float c){ glVertexAttrib3f(i,a,b,c); }
SF void sf_glVertexAttrib4f(unsigned i,float a,float b,float c,float d){ glVertexAttrib4f(i,a,b,c,d); }
