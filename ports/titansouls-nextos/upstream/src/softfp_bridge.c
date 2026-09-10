#include "softfp_bridge.h"

#include <math.h>
#include <stdlib.h>

double ASM2_GUEST_PCS asm2_sf_acos(double value) { return acos(value); }
float ASM2_GUEST_PCS asm2_sf_acosf(float value) { return acosf(value); }
double ASM2_GUEST_PCS asm2_sf_asin(double value) { return asin(value); }
float ASM2_GUEST_PCS asm2_sf_asinf(float value) { return asinf(value); }
double ASM2_GUEST_PCS asm2_sf_atan(double value) { return atan(value); }
float ASM2_GUEST_PCS asm2_sf_atanf(float value) { return atanf(value); }
double ASM2_GUEST_PCS asm2_sf_atan2(double y, double x) { return atan2(y, x); }
float ASM2_GUEST_PCS asm2_sf_atan2f(float y, float x) { return atan2f(y, x); }
double ASM2_GUEST_PCS asm2_sf_ceil(double value) { return ceil(value); }
float ASM2_GUEST_PCS asm2_sf_ceilf(float value) { return ceilf(value); }
double ASM2_GUEST_PCS asm2_sf_cos(double value) { return cos(value); }
float ASM2_GUEST_PCS asm2_sf_cosf(float value) { return cosf(value); }
double ASM2_GUEST_PCS asm2_sf_difftime(int32_t end, int32_t beginning) {
  return difftime((time_t)end, (time_t)beginning);
}
double ASM2_GUEST_PCS asm2_sf_exp(double value) { return exp(value); }
float ASM2_GUEST_PCS asm2_sf_expf(float value) { return expf(value); }
double ASM2_GUEST_PCS asm2_sf_floor(double value) { return floor(value); }
float ASM2_GUEST_PCS asm2_sf_floorf(float value) { return floorf(value); }
double ASM2_GUEST_PCS asm2_sf_fmod(double numerator, double denominator) {
  return fmod(numerator, denominator);
}
float ASM2_GUEST_PCS asm2_sf_fmodf(float numerator, float denominator) {
  return fmodf(numerator, denominator);
}
double ASM2_GUEST_PCS asm2_sf_ldexp(double value, int exponent) {
  return ldexp(value, exponent);
}
double ASM2_GUEST_PCS asm2_sf_log(double value) { return log(value); }
float ASM2_GUEST_PCS asm2_sf_logf(float value) { return logf(value); }
double ASM2_GUEST_PCS asm2_sf_log10(double value) { return log10(value); }
float ASM2_GUEST_PCS asm2_sf_modff(float value, float *integer_part) {
  return modff(value, integer_part);
}
double ASM2_GUEST_PCS asm2_sf_pow(double base, double exponent) {
  return pow(base, exponent);
}
float ASM2_GUEST_PCS asm2_sf_powf(float base, float exponent) {
  return powf(base, exponent);
}
double ASM2_GUEST_PCS asm2_sf_sin(double value) { return sin(value); }
float ASM2_GUEST_PCS asm2_sf_sinf(float value) { return sinf(value); }
double ASM2_GUEST_PCS asm2_sf_sinh(double value) { return sinh(value); }
double ASM2_GUEST_PCS asm2_sf_strtod(const char *text, char **end) {
  return strtod(text, end);
}
double ASM2_GUEST_PCS asm2_sf_frexp(double value, int *exponent) {
  return frexp(value, exponent);
}
float ASM2_GUEST_PCS asm2_sf_log10f(float value) { return log10f(value); }
long ASM2_GUEST_PCS asm2_sf_lrintf(float value) { return lrintf(value); }
double ASM2_GUEST_PCS asm2_sf_rint(double value) { return rint(value); }
float ASM2_GUEST_PCS asm2_sf_sqrtf(float value) { return sqrtf(value); }
double ASM2_GUEST_PCS asm2_sf_sqrt(double value) { return sqrt(value); }
double ASM2_GUEST_PCS asm2_sf_atof(const char *text) { return atof(text); }
float ASM2_GUEST_PCS asm2_sf_strtof(const char *text, char **end) {
  return strtof(text, end);
}
double ASM2_GUEST_PCS asm2_sf_tan(double value) { return tan(value); }
float ASM2_GUEST_PCS asm2_sf_tanf(float value) { return tanf(value); }

void ASM2_GUEST_PCS asm2_sf_glBlendColor(GLfloat red, GLfloat green,
                                          GLfloat blue, GLfloat alpha) {
  glBlendColor(red, green, blue, alpha);
}
void ASM2_GUEST_PCS asm2_sf_glClearColor(GLfloat red, GLfloat green,
                                          GLfloat blue, GLfloat alpha) {
  glClearColor(red, green, blue, alpha);
}
void ASM2_GUEST_PCS asm2_sf_glClearDepthf(GLfloat depth) {
  glClearDepthf(depth);
}
void ASM2_GUEST_PCS asm2_sf_glDepthRangef(GLfloat near_value,
                                           GLfloat far_value) {
  glDepthRangef(near_value, far_value);
}
void ASM2_GUEST_PCS asm2_sf_glLineWidth(GLfloat width) { glLineWidth(width); }
void ASM2_GUEST_PCS asm2_sf_glPolygonOffset(GLfloat factor, GLfloat units) {
  glPolygonOffset(factor, units);
}
void ASM2_GUEST_PCS asm2_sf_glSampleCoverage(GLfloat value,
                                              GLboolean invert) {
  glSampleCoverage(value, invert);
}
void ASM2_GUEST_PCS asm2_sf_glTexParameterf(GLenum target, GLenum name,
                                             GLfloat value) {
  glTexParameterf(target, name, value);
}
void ASM2_GUEST_PCS asm2_sf_glUniform1f(GLint location, GLfloat value) {
  /* instrumentado: ver [uni] no log com TS_UDUMP=1 */
  extern void ts_uni1f_probe(int loc, float v);
  ts_uni1f_probe(location, value);
}
void ASM2_GUEST_PCS asm2_sf_glVertexAttrib4f(GLuint index, GLfloat x,
                                              GLfloat y, GLfloat z,
                                              GLfloat w) {
  glVertexAttrib4f(index, x, y, z, w);
}
