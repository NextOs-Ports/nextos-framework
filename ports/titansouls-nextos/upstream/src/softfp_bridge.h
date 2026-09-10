#ifndef ASM2_SOFTFP_BRIDGE_H
#define ASM2_SOFTFP_BRIDGE_H

#include <GLES2/gl2.h>
#include <stdint.h>
#include <time.h>

#include "bionic_compat.h"

double ASM2_GUEST_PCS asm2_sf_acos(double value);
float ASM2_GUEST_PCS asm2_sf_acosf(float value);
double ASM2_GUEST_PCS asm2_sf_asin(double value);
float ASM2_GUEST_PCS asm2_sf_asinf(float value);
double ASM2_GUEST_PCS asm2_sf_atan(double value);
float ASM2_GUEST_PCS asm2_sf_atanf(float value);
double ASM2_GUEST_PCS asm2_sf_atan2(double y, double x);
float ASM2_GUEST_PCS asm2_sf_atan2f(float y, float x);
double ASM2_GUEST_PCS asm2_sf_ceil(double value);
float ASM2_GUEST_PCS asm2_sf_ceilf(float value);
double ASM2_GUEST_PCS asm2_sf_cos(double value);
float ASM2_GUEST_PCS asm2_sf_cosf(float value);
double ASM2_GUEST_PCS asm2_sf_difftime(int32_t end, int32_t beginning);
double ASM2_GUEST_PCS asm2_sf_exp(double value);
float ASM2_GUEST_PCS asm2_sf_expf(float value);
double ASM2_GUEST_PCS asm2_sf_floor(double value);
float ASM2_GUEST_PCS asm2_sf_floorf(float value);
double ASM2_GUEST_PCS asm2_sf_fmod(double numerator, double denominator);
float ASM2_GUEST_PCS asm2_sf_fmodf(float numerator, float denominator);
double ASM2_GUEST_PCS asm2_sf_ldexp(double value, int exponent);
double ASM2_GUEST_PCS asm2_sf_log(double value);
float ASM2_GUEST_PCS asm2_sf_logf(float value);
double ASM2_GUEST_PCS asm2_sf_log10(double value);
float ASM2_GUEST_PCS asm2_sf_modff(float value, float *integer_part);
double ASM2_GUEST_PCS asm2_sf_pow(double base, double exponent);
float ASM2_GUEST_PCS asm2_sf_powf(float base, float exponent);
double ASM2_GUEST_PCS asm2_sf_sin(double value);
float ASM2_GUEST_PCS asm2_sf_sinf(float value);
double ASM2_GUEST_PCS asm2_sf_sinh(double value);
double ASM2_GUEST_PCS asm2_sf_strtod(const char *text, char **end);
double ASM2_GUEST_PCS asm2_sf_frexp(double value, int *exponent);
float ASM2_GUEST_PCS asm2_sf_log10f(float value);
long ASM2_GUEST_PCS asm2_sf_lrintf(float value);
double ASM2_GUEST_PCS asm2_sf_rint(double value);
float ASM2_GUEST_PCS asm2_sf_sqrtf(float value);
double ASM2_GUEST_PCS asm2_sf_sqrt(double value);
double ASM2_GUEST_PCS asm2_sf_atof(const char *text);
float ASM2_GUEST_PCS asm2_sf_strtof(const char *text, char **end);

double ASM2_GUEST_PCS asm2_sf_tan(double value);
float ASM2_GUEST_PCS asm2_sf_tanf(float value);

void ASM2_GUEST_PCS asm2_sf_glBlendColor(GLfloat red, GLfloat green,
                                          GLfloat blue, GLfloat alpha);
void ASM2_GUEST_PCS asm2_sf_glClearColor(GLfloat red, GLfloat green,
                                          GLfloat blue, GLfloat alpha);
void ASM2_GUEST_PCS asm2_sf_glClearDepthf(GLfloat depth);
void ASM2_GUEST_PCS asm2_sf_glDepthRangef(GLfloat near_value,
                                           GLfloat far_value);
void ASM2_GUEST_PCS asm2_sf_glLineWidth(GLfloat width);
void ASM2_GUEST_PCS asm2_sf_glPolygonOffset(GLfloat factor, GLfloat units);
void ASM2_GUEST_PCS asm2_sf_glSampleCoverage(GLfloat value,
                                              GLboolean invert);
void ASM2_GUEST_PCS asm2_sf_glTexParameterf(GLenum target, GLenum name,
                                             GLfloat value);
void ASM2_GUEST_PCS asm2_sf_glUniform1f(GLint location, GLfloat value);
void ASM2_GUEST_PCS asm2_sf_glVertexAttrib4f(GLuint index, GLfloat x,
                                              GLfloat y, GLfloat z,
                                              GLfloat w);

#endif
