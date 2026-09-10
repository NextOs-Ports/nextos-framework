/* softfp_shim.h -- softfp(game) -> hardfp(host) float-ABI bridge for KOTOR.
 * libm + GLES2 float-by-value entry points. */
#ifndef __SOFTFP_SHIM_H__
#define __SOFTFP_SHIM_H__

#define SF __attribute__((pcs("aapcs")))

#include "so_util.h"

/* returns a softfp wrapper for a libm symbol name, or NULL. */
void *softfp_resolve(const char *nm);

/* number of libm softfp thunks, and a filler into a DynLibFunction table. */
int softfp_table_count(void);
int softfp_fill_table(DynLibFunction *dst);

/* GLES2 float-by-value entry points (softfp wrappers -> native hardfp GL). */
SF void sf_glClearColor(float r, float g, float b, float a);
SF void sf_glClearDepthf(float d);
SF void sf_glDepthRangef(float n, float f);
SF void sf_glBlendColor(float r, float g, float b, float a);
SF void sf_glLineWidth(float w);
SF void sf_glPolygonOffset(float factor, float units);
SF void sf_glSampleCoverage(float value, unsigned char invert);
SF void sf_glTexParameterf(unsigned target, unsigned pname, float param);
SF void sf_glUniform1f(int loc, float v0);
SF void sf_glUniform2f(int loc, float v0, float v1);
SF void sf_glUniform3f(int loc, float v0, float v1, float v2);
SF void sf_glUniform4f(int loc, float v0, float v1, float v2, float v3);
SF void sf_glVertexAttrib1f(unsigned i, float v0);
SF void sf_glVertexAttrib2f(unsigned i, float v0, float v1);
SF void sf_glVertexAttrib3f(unsigned i, float v0, float v1, float v2);
SF void sf_glVertexAttrib4f(unsigned i, float v0, float v1, float v2, float v3);
SF void sf_sincosf(float x, float *s, float *c);
SF void sf_sincos(double x, double *s, double *c);

#endif
