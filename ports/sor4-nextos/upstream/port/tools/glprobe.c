#include <stdio.h>
#include <stdint.h>
typedef void* SDL_Window; typedef void* SDL_GLContext;
extern int SDL_Init(uint32_t);
extern const char* SDL_GetError(void);
extern const char* SDL_GetCurrentVideoDriver(void);
extern int SDL_GL_SetAttribute(int,int);
extern SDL_Window* SDL_CreateWindow(const char*,int,int,int,int,uint32_t);
extern SDL_GLContext SDL_GL_CreateContext(SDL_Window*);
extern void* SDL_GL_GetProcAddress(const char*);
typedef const unsigned char* (*GS)(unsigned int);
typedef void (*GIV)(unsigned int,int*);
int main(){
  if(SDL_Init(0x20)){ printf("init fail: %s\n",SDL_GetError()); return 1; } // SDL_INIT_VIDEO=0x20
  printf("video driver: %s\n", SDL_GetCurrentVideoDriver());
  SDL_GL_SetAttribute(21,1); // SDL_GL_CONTEXT_PROFILE_MASK=21, COMPAT=1? (2 in SDL) -- try anyway
  SDL_Window* w=SDL_CreateWindow("p",0,0,640,480, 0x2); // OPENGL only
  if(!w){ printf("win fail: %s\n",SDL_GetError()); return 1; }
  SDL_GLContext c=SDL_GL_CreateContext(w);
  if(!c){ printf("ctx fail: %s\n",SDL_GetError()); return 1; }
  GS gs=(GS)SDL_GL_GetProcAddress("glGetString");
  if(!gs){ printf("no glGetString\n"); return 1; }
  printf("VENDOR  : %s\n", gs(0x1F00));
  printf("RENDERER: %s\n", gs(0x1F01));
  printf("VERSION : %s\n", gs(0x1F02));
  const char* ext=(const char*)gs(0x1F03);
  printf("EXT= %.700s\n", ext?ext:"(null)");
  GIV giv=(GIV)SDL_GL_GetProcAddress("glGetIntegerv");
  int n=-1; if(giv) giv(0x821D,&n); printf("NUM_EXT(GL3)=%d\n", n);
  printf("glGenFramebuffers=%p glGenFramebuffersEXT=%p glGetStringi=%p\n",
     SDL_GL_GetProcAddress("glGenFramebuffers"), SDL_GL_GetProcAddress("glGenFramebuffersEXT"), SDL_GL_GetProcAddress("glGetStringi"));
  return 0;
}
