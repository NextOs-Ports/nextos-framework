/* sor4splash.c -- renderer PROPRIO de tela de bake (SEM progressor, que trava).
 * SDL2 do SISTEMA + GLES2 (igual etc2probe, que rodou liso no R36S como cliente Wayland).
 * Mostra uma IMAGEM de fundo (loading screen, com a mensagem ja embutida por idioma) + uma
 * BARRA DE PROGRESSO, lendo um arquivo de controle que o bake atualiza.
 *
 * Uso: sor4splash <img.rgba> <W> <H> <controlfile> <stopfile>
 *   img.rgba = imagem RGBA crua WxH (loading screen do idioma certo, gerada no host).
 *   controlfile: 1a linha "<state> <cur> <total>" (barra = cur/total). Atualizado por um poller.
 *   stopfile: quando existir, o splash sai.
 * Roda ANTES do LD_LIBRARY_PATH=$PKG/libs (pega o SDL2 do sistema -> backend certo:
 *   Wayland no R36S/X5M, fbdev no Mali-450). SDL2 is loaded with dlopen so the
 *   binary stays backend-neutral. Build: gcc sor4splash.c -ldl -o sor4splash. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>

typedef void* SDLW;
static int (*SDL_Init)(uint32_t); static const char* (*SDL_GetError)(void);
static void (*SDL_Quit)(void); static int (*SDL_GL_SetAttribute)(int,int);
static SDLW* (*SDL_CreateWindow)(const char*,int,int,int,int,uint32_t);
static void* (*SDL_GL_CreateContext)(SDLW*); static int (*SDL_GL_SetSwapInterval)(int);
static void (*SDL_GL_SwapWindow)(SDLW*); static void* (*SDL_GL_GetProcAddress)(const char*);
static void (*SDL_Delay)(uint32_t); static int (*SDL_PollEvent)(void*);
static void (*SDL_GL_GetDrawableSize)(SDLW*,int*,int*); static void (*SDL_ShowWindow)(SDLW*);
static void (*SDL_RaiseWindow)(SDLW*);

static int load_sdl(void){
  void* lib=dlopen("libSDL2-2.0.so.0",RTLD_NOW|RTLD_GLOBAL);
  if(!lib) lib=dlopen("libSDL2.so",RTLD_NOW|RTLD_GLOBAL);
  if(!lib){ fprintf(stderr,"SDL2: %s\n",dlerror()); return 0; }
#define SDLGET(n) do { *(void**)&n=dlsym(lib,#n); if(!n){ fprintf(stderr,"missing %s\n",#n); return 0; } } while(0)
  SDLGET(SDL_Init);SDLGET(SDL_GetError);SDLGET(SDL_Quit);SDLGET(SDL_GL_SetAttribute);
  SDLGET(SDL_CreateWindow);SDLGET(SDL_GL_CreateContext);SDLGET(SDL_GL_SetSwapInterval);
  SDLGET(SDL_GL_SwapWindow);SDLGET(SDL_GL_GetProcAddress);SDLGET(SDL_Delay);
  SDLGET(SDL_PollEvent);SDLGET(SDL_GL_GetDrawableSize);SDLGET(SDL_ShowWindow);SDLGET(SDL_RaiseWindow);
#undef SDLGET
  return 1;
}

typedef unsigned int GLenum; typedef int GLint; typedef unsigned int GLuint; typedef int GLsizei;
typedef float GLfloat; typedef char GLchar; typedef unsigned char GLubyte;
static GLuint (*glCreateShader)(GLenum); static void (*glShaderSource)(GLuint,GLsizei,const GLchar*const*,const GLint*);
static void (*glCompileShader)(GLuint); static GLuint (*glCreateProgram)(void); static void (*glAttachShader)(GLuint,GLuint);
static void (*glLinkProgram)(GLuint); static void (*glUseProgram)(GLuint);
static GLint (*glGetAttribLocation)(GLuint,const GLchar*); static GLint (*glGetUniformLocation)(GLuint,const GLchar*);
static void (*glUniform1i)(GLint,GLint); static void (*glUniform4f)(GLint,GLfloat,GLfloat,GLfloat,GLfloat);
static void (*glGenBuffers)(GLsizei,GLuint*); static void (*glBindBuffer)(GLenum,GLuint);
static void (*glBufferData)(GLenum,long,const void*,GLenum);
static void (*glVertexAttribPointer)(GLuint,GLint,GLenum,GLubyte,GLsizei,const void*); static void (*glEnableVertexAttribArray)(GLuint);
static void (*glGenTextures)(GLsizei,GLuint*); static void (*glBindTexture)(GLenum,GLuint);
static void (*glTexImage2D)(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const void*); static void (*glTexParameteri)(GLenum,GLenum,GLint);
static void (*glActiveTexture)(GLenum); static void (*glViewport)(GLint,GLint,GLsizei,GLsizei);
static void (*glClearColor)(GLfloat,GLfloat,GLfloat,GLfloat); static void (*glClear)(GLenum); static void (*glDrawArrays)(GLenum,GLint,GLsizei);
#define GET(n) (*(void**)&n = SDL_GL_GetProcAddress(#n))
enum { GL_TEXTURE_2D=0x0DE1,GL_RGBA=0x1908,GL_UBYTE=0x1401,GL_NEAREST=0x2600,GL_LINEAR=0x2601,
  GL_TEX_MIN=0x2801,GL_TEX_MAG=0x2800,GL_WRAP_S=0x2802,GL_WRAP_T=0x2803,GL_CLAMP=0x812F,
  GL_VTX=0x8B31,GL_FRAG=0x8B30,GL_ARRAY=0x8892,GL_STATIC=0x88E4,GL_FLOAT=0x1406,GL_TRISTRIP=0x0005,
  GL_COLORBIT=0x4000,GL_TEX0=0x84C0 };
static GLuint mksh(GLenum t,const char*s){ GLuint sh=glCreateShader(t); glShaderSource(sh,1,&s,0); glCompileShader(sh); return sh; }

int main(int argc,char**argv){
  if(argc<6){ fprintf(stderr,"uso: sor4splash <img.rgba> <W> <H> <control> <stop>\n"); return 2; }
  const char* imgpath=argv[1]; int IW=atoi(argv[2]), IH=atoi(argv[3]);
  const char* ctlpath=argv[4]; const char* stoppath=argv[5];

  if(!load_sdl()) return 1;
  /* carrega a imagem RGBA crua */
  unsigned char* img=(unsigned char*)malloc((size_t)IW*IH*4);
  FILE* f=fopen(imgpath,"rb"); if(!f||!img){ fprintf(stderr,"img fail\n"); return 2; }
  if(fread(img,1,(size_t)IW*IH*4,f)!=(size_t)IW*IH*4){ fprintf(stderr,"img short\n"); }
  fclose(f);

  if(SDL_Init(0x20)){ fprintf(stderr,"init: %s\n",SDL_GetError()); return 1; }
  SDL_GL_SetAttribute(21,4); SDL_GL_SetAttribute(17,2); SDL_GL_SetAttribute(18,0); SDL_GL_SetAttribute(5,1);
  SDLW* w=SDL_CreateWindow("SOR4",0,0,640,480,0x2/*OPENGL*/|0x1001/*FULLSCREEN_DESKTOP*/);
  if(!w){ fprintf(stderr,"win: %s\n",SDL_GetError()); return 1; }
  if(!SDL_GL_CreateContext(w)){ fprintf(stderr,"ctx: %s\n",SDL_GetError()); return 1; }
  SDL_ShowWindow(w); SDL_RaiseWindow(w);
  SDL_GL_SetSwapInterval(1);
  int DW=640,DH=480; SDL_GL_GetDrawableSize(w,&DW,&DH);
  fprintf(stderr,"sor4splash: drawable %dx%d\n", DW, DH);
  GET(glCreateShader);GET(glShaderSource);GET(glCompileShader);GET(glCreateProgram);GET(glAttachShader);
  GET(glLinkProgram);GET(glUseProgram);GET(glGetAttribLocation);GET(glGetUniformLocation);GET(glUniform1i);
  GET(glUniform4f);GET(glGenBuffers);GET(glBindBuffer);GET(glBufferData);GET(glVertexAttribPointer);
  GET(glEnableVertexAttribArray);GET(glGenTextures);GET(glBindTexture);GET(glTexImage2D);GET(glTexParameteri);
  GET(glActiveTexture);GET(glViewport);GET(glClearColor);GET(glClear);GET(glDrawArrays);

  GLuint tex; glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_2D,tex);
  glTexParameteri(GL_TEXTURE_2D,GL_TEX_MIN,GL_LINEAR); glTexParameteri(GL_TEXTURE_2D,GL_TEX_MAG,GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D,GL_WRAP_S,GL_CLAMP); glTexParameteri(GL_TEXTURE_2D,GL_WRAP_T,GL_CLAMP);
  glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,IW,IH,0,GL_RGBA,GL_UBYTE,img);

  const char* vs="attribute vec2 aPos;attribute vec2 aUV;varying vec2 vUV;void main(){vUV=aUV;gl_Position=vec4(aPos,0.0,1.0);}";
  const char* fs="precision mediump float;uniform sampler2D t;uniform int uTex;uniform vec4 uCol;varying vec2 vUV;"
                 "void main(){ if(uTex==1) gl_FragColor=texture2D(t,vUV); else gl_FragColor=uCol; }";
  GLuint p=glCreateProgram(); glAttachShader(p,mksh(GL_VTX,vs)); glAttachShader(p,mksh(GL_FRAG,fs)); glLinkProgram(p); glUseProgram(p);
  GLint aPos=glGetAttribLocation(p,"aPos"), aUV=glGetAttribLocation(p,"aUV");
  GLint uTex=glGetUniformLocation(p,"uTex"), uCol=glGetUniformLocation(p,"uCol");
  glUniform1i(glGetUniformLocation(p,"t"),0);

  /* quad fullscreen (img). UV.v invertido (img row0 = topo). */
  GLfloat full[]={ -1,-1, 0,1,  1,-1, 1,1,  -1,1, 0,0,  1,1, 1,0 };
  GLuint vb,vb2; glGenBuffers(1,&vb); glBindBuffer(GL_ARRAY,vb); glBufferData(GL_ARRAY,sizeof full,full,GL_STATIC);
  glGenBuffers(1,&vb2);
  glActiveTexture(GL_TEX0); glBindTexture(GL_TEXTURE_2D,tex);

  /* loop ate o stopfile */
  for(int frame=0;;frame++){
    /* sai se o stopfile existir */
    if(access(stoppath,0)==0) break;
    /* le o progresso: "<state> <cur> <total>" */
    long cur=0,total=1; { FILE* cf=fopen(ctlpath,"r"); if(cf){ int st; if(fscanf(cf,"%d %ld %ld",&st,&cur,&total)<3){cur=0;total=1;} fclose(cf);} }
    if(total<=0) total=1; if(cur>total)cur=total; float frac=(float)cur/(float)total;

    /* drena eventos p/ o compositor nao matar (sem processar) */
    char ev[256]; while(SDL_PollEvent(ev)){}

    glViewport(0,0,DW,DH); glClearColor(0,0,0,1); glClear(GL_COLORBIT);
    /* imagem de fundo */
    glBindBuffer(GL_ARRAY,vb);
    glVertexAttribPointer(aPos,2,GL_FLOAT,0,16,(void*)0); glEnableVertexAttribArray(aPos);
    glVertexAttribPointer(aUV,2,GL_FLOAT,0,16,(void*)8); glEnableVertexAttribArray(aUV);
    glUniform1i(uTex,1); glDrawArrays(GL_TRISTRIP,0,4);

    /* barra de progresso (faixa inferior): trilho escuro + preenchimento verde proporcional */
    float by0=-0.93f, by1=-0.86f, bx0=-0.90f, bx1=0.90f;
    GLfloat trk[]={ bx0,by0,0,0, bx1,by0,0,0, bx0,by1,0,0, bx1,by1,0,0 };
    glBindBuffer(GL_ARRAY,vb2); glBufferData(GL_ARRAY,sizeof trk,trk,GL_STATIC);
    glVertexAttribPointer(aPos,2,GL_FLOAT,0,16,(void*)0); glEnableVertexAttribArray(aPos);
    glUniform1i(uTex,0); glUniform4f(uCol,0.08f,0.08f,0.08f,1.0f); glDrawArrays(GL_TRISTRIP,0,4);
    float fx1=bx0+(bx1-bx0)*frac;
    GLfloat fil[]={ bx0,by0,0,0, fx1,by0,0,0, bx0,by1,0,0, fx1,by1,0,0 };
    glBufferData(GL_ARRAY,sizeof fil,fil,GL_STATIC);
    glVertexAttribPointer(aPos,2,GL_FLOAT,0,16,(void*)0); glEnableVertexAttribArray(aPos);
    glUniform4f(uCol,0.20f,0.85f,0.30f,1.0f); glDrawArrays(GL_TRISTRIP,0,4);

    SDL_GL_SwapWindow(w);
    SDL_Delay(60);
  }
  free(img);
  SDL_Quit();
  return 0;
}
