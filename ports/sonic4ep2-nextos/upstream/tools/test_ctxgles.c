/* valida a logica ctx_is_gles do egl_shim: rejeitar desktop-GL, aceitar GLES */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>
static const unsigned char*(*gs)(unsigned)=0;
static int ctx_is_gles(void){
  if(!gs) return 1;
  const char*v=(const char*)gs(0x1F02);
  if(!v) return 1;
  return (strstr(v,"ES")!=0)||(strstr(v,"es")!=0);
}
int main(void){
  if(SDL_Init(SDL_INIT_VIDEO)!=0){ printf("SDL_Init FAIL: %s\n",SDL_GetError()); return 2; }
  SDL_Window*w=SDL_CreateWindow("t",0,0,64,64,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
  if(!w){ printf("CreateWindow FAIL: %s\n",SDL_GetError()); return 3; }
  /* caso 1: pede DESKTOP GL (core 3.3) */
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
  SDL_GLContext c1=SDL_GL_CreateContext(w);
  gs=(const unsigned char*(*)(unsigned))SDL_GL_GetProcAddress("glGetString");
  if(c1){ int r=ctx_is_gles(); printf("DESKTOP(core3.3): GL_VERSION=\"%s\" ctx_is_gles=%d [esperado 0 -> %s]\n",
          gs?(const char*)gs(0x1F02):"?", r, r==0?"OK REJEITA":"FALHA"); SDL_GL_DeleteContext(c1);}
  else printf("DESKTOP(core3.3): sem contexto (%s)\n",SDL_GetError());
  /* caso 2: pede GLES2 */
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,0);
  SDL_GLContext c2=SDL_GL_CreateContext(w);
  gs=(const unsigned char*(*)(unsigned))SDL_GL_GetProcAddress("glGetString");
  if(c2){ int r=ctx_is_gles(); printf("GLES(es2):      GL_VERSION=\"%s\" ctx_is_gles=%d [esperado 1 -> %s]\n",
          gs?(const char*)gs(0x1F02):"?", r, r==1?"OK ACEITA":"FALHA"); SDL_GL_DeleteContext(c2);}
  else printf("GLES(es2): sem contexto (%s)\n",SDL_GetError());
  SDL_DestroyWindow(w); SDL_Quit(); return 0;
}
