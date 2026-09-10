/* softfp_shim.c -- ponte de ABI float (armv7).
   O libShantaeCurse_android.so é SOFTFP (base AAPCS, Tag_ABI_VFP_args ausente):
   double/float passam em registradores INTEIROS (r0:r1, r0..r3). O glibc
   libm/libc E o libGLESv2 do device são HARDFP (Tag_ABI_VFP_args=VFP): float/
   double em d0..d7/s0..s3. Chamar essas libs direto do código softfp do jogo
   passa os args nos registradores errados -> lixo (ex: glClearColor recebeu
   (0,1280,0,720) = dims em vez de cor -> clear errado -> tela preta).

   Fix: wrappers declarados pcs("aapcs") (softfp) -> recebem como o jogo manda e
   chamam a função real (hardfp); o GCC faz a tradução de registradores. Roteados
   via softfp_resolve() ANTES do dlsym(RTLD_DEFAULT) no so_resolve. */
#include <dlfcn.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SF __attribute__((pcs("aapcs")))

/* 1 arg double->double */
#define W1D(n)  SF double sf_##n(double x){ return n(x); }
/* 1 arg float->float */
#define W1F(n)  SF float  sf_##n(float x){ return n(x); }
/* 2 arg double,double->double */
#define W2D(n)  SF double sf_##n(double a,double b){ return n(a,b); }
/* 2 arg float,float->float */
#define W2F(n)  SF float  sf_##n(float a,float b){ return n(a,b); }

W1D(acos) W1D(asin) W1D(atan) W1D(cos) W1D(sin) W1D(tan)
W1D(cosh) W1D(sinh) W1D(tanh)
W1D(exp) W1D(exp2) W1D(log) W1D(log10) W1D(sqrt)
W1D(ceil) W1D(floor) W1D(round) W1D(trunc) W1D(rint)
W1F(acosf) W1F(asinf) W1F(atanf) W1F(cosf) W1F(sinf) W1F(tanf)
W1F(expf) W1F(logf) W1F(sqrtf) W1F(fabsf) W1F(ceilf) W1F(floorf) W1F(roundf) W1F(truncf)
W2D(atan2) W2D(fmod) W2D(pow) W2D(remainder)
W2F(atan2f) W2F(fmodf) W2F(powf)

SF double sf_modf(double x,double *iptr){ return modf(x,iptr); }
SF float  sf_modff(float x,float *iptr){ return modff(x,iptr); }
SF double sf_frexp(double x,int *e){ return frexp(x,e); }
SF double sf_ldexp(double x,int e){ return ldexp(x,e); }
SF double sf_strtod(const char *s,char **end){ return strtod(s,end); }

/* ---- GL float-by-value (chama o glClearColor HARDFP do device) ---- */
SF void sf_glClearColor(float r,float g,float b,float a){
  static void(*real)(float,float,float,float)=0;
  if(!real) real=(void(*)(float,float,float,float))dlsym(RTLD_DEFAULT,"glClearColor");
  if(real) real(r,g,b,a);
}
SF void sf_glClearDepthf(float d){
  static void(*real)(float)=0;
  if(!real) real=(void(*)(float))dlsym(RTLD_DEFAULT,"glClearDepthf");
  if(real) real(d);
}
SF void sf_glDepthRangef(float n,float f){
  static void(*real)(float,float)=0;
  if(!real) real=(void(*)(float,float))dlsym(RTLD_DEFAULT,"glDepthRangef");
  if(real) real(n,f);
}
SF void sf_glLineWidth(float w){
  static void(*real)(float)=0;
  if(!real) real=(void(*)(float))dlsym(RTLD_DEFAULT,"glLineWidth");
  if(real) real(w);
}
SF void sf_glPolygonOffset(float factor,float units){
  static void(*real)(float,float)=0;
  if(!real) real=(void(*)(float,float))dlsym(RTLD_DEFAULT,"glPolygonOffset");
  if(real) real(factor,units);
}
SF void sf_glSampleCoverage(float value,unsigned char invert){
  static void(*real)(float,unsigned char)=0;
  if(!real) real=(void(*)(float,unsigned char))dlsym(RTLD_DEFAULT,"glSampleCoverage");
  if(real) real(value,invert);
}
SF void sf_glBlendColor(float r,float g,float b,float a){
  static void(*real)(float,float,float,float)=0;
  if(!real) real=(void(*)(float,float,float,float))dlsym(RTLD_DEFAULT,"glBlendColor");
  if(real) real(r,g,b,a);
}
SF void sf_glTexParameterf(unsigned target,unsigned pname,float param){
  static void(*real)(unsigned,unsigned,float)=0;
  if(!real) real=(void(*)(unsigned,unsigned,float))dlsym(RTLD_DEFAULT,"glTexParameterf");
  if(real) real(target,pname,param);
}
SF void sf_glUniform1f(int loc,float v0){
  static void(*real)(int,float)=0;
  if(!real) real=(void(*)(int,float))dlsym(RTLD_DEFAULT,"glUniform1f");
  if(real) real(loc,v0);
}
SF void sf_glUniform2f(int loc,float v0,float v1){
  static void(*real)(int,float,float)=0;
  if(!real) real=(void(*)(int,float,float))dlsym(RTLD_DEFAULT,"glUniform2f");
  if(real) real(loc,v0,v1);
}
SF void sf_glUniform3f(int loc,float v0,float v1,float v2){
  static void(*real)(int,float,float,float)=0;
  if(!real) real=(void(*)(int,float,float,float))dlsym(RTLD_DEFAULT,"glUniform3f");
  if(real) real(loc,v0,v1,v2);
}
SF void sf_glUniform4f(int loc,float v0,float v1,float v2,float v3){
  static void(*real)(int,float,float,float,float)=0;
  if(!real) real=(void(*)(int,float,float,float,float))dlsym(RTLD_DEFAULT,"glUniform4f");
  if(real) real(loc,v0,v1,v2,v3);
}
SF void sf_glVertexAttrib1f(unsigned idx,float x){
  static void(*real)(unsigned,float)=0;
  if(!real) real=(void(*)(unsigned,float))dlsym(RTLD_DEFAULT,"glVertexAttrib1f");
  if(real) real(idx,x);
}
SF void sf_glVertexAttrib2f(unsigned idx,float x,float y){
  static void(*real)(unsigned,float,float)=0;
  if(!real) real=(void(*)(unsigned,float,float))dlsym(RTLD_DEFAULT,"glVertexAttrib2f");
  if(real) real(idx,x,y);
}
SF void sf_glVertexAttrib3f(unsigned idx,float x,float y,float z){
  static void(*real)(unsigned,float,float,float)=0;
  if(!real) real=(void(*)(unsigned,float,float,float))dlsym(RTLD_DEFAULT,"glVertexAttrib3f");
  if(real) real(idx,x,y,z);
}
SF void sf_glVertexAttrib4f(unsigned idx,float x,float y,float z,float w){
  static void(*real)(unsigned,float,float,float,float)=0;
  if(!real) real=(void(*)(unsigned,float,float,float,float))dlsym(RTLD_DEFAULT,"glVertexAttrib4f");
  if(real) real(idx,x,y,z,w);
}

struct sfent { const char *nm; void *fn; };
static const struct sfent SFTAB[] = {
  {"acos",sf_acos},{"asin",sf_asin},{"atan",sf_atan},{"cos",sf_cos},{"sin",sf_sin},{"tan",sf_tan},
  {"cosh",sf_cosh},{"sinh",sf_sinh},{"tanh",sf_tanh},
  {"exp",sf_exp},{"exp2",sf_exp2},{"log",sf_log},{"log10",sf_log10},{"sqrt",sf_sqrt},
  {"ceil",sf_ceil},{"floor",sf_floor},{"round",sf_round},{"trunc",sf_trunc},{"rint",sf_rint},
  {"acosf",sf_acosf},{"asinf",sf_asinf},{"atanf",sf_atanf},{"cosf",sf_cosf},{"sinf",sf_sinf},{"tanf",sf_tanf},
  {"expf",sf_expf},{"logf",sf_logf},{"sqrtf",sf_sqrtf},{"fabsf",sf_fabsf},
  {"ceilf",sf_ceilf},{"floorf",sf_floorf},{"roundf",sf_roundf},{"truncf",sf_truncf},
  {"atan2",sf_atan2},{"fmod",sf_fmod},{"pow",sf_pow},{"remainder",sf_remainder},
  {"atan2f",sf_atan2f},{"fmodf",sf_fmodf},{"powf",sf_powf},
  {"modf",sf_modf},{"modff",sf_modff},{"frexp",sf_frexp},{"ldexp",sf_ldexp},{"strtod",sf_strtod},
  {"glClearColor",sf_glClearColor},
  {"glClearDepthf",sf_glClearDepthf},
  {"glDepthRangef",sf_glDepthRangef},
  {"glLineWidth",sf_glLineWidth},
  {"glPolygonOffset",sf_glPolygonOffset},
  {"glSampleCoverage",sf_glSampleCoverage},
  {"glBlendColor",sf_glBlendColor},
  {"glTexParameterf",sf_glTexParameterf},
  {"glUniform1f",sf_glUniform1f},
  {"glUniform2f",sf_glUniform2f},
  {"glUniform3f",sf_glUniform3f},
  {"glUniform4f",sf_glUniform4f},
  {"glVertexAttrib1f",sf_glVertexAttrib1f},
  {"glVertexAttrib2f",sf_glVertexAttrib2f},
  {"glVertexAttrib3f",sf_glVertexAttrib3f},
  {"glVertexAttrib4f",sf_glVertexAttrib4f},
};

void *softfp_resolve(const char *nm){
  if(!nm) return 0;
  for(unsigned i=0;i<sizeof(SFTAB)/sizeof(SFTAB[0]);i++)
    if(!strcmp(nm,SFTAB[i].nm)) return SFTAB[i].fn;
  return 0;
}
