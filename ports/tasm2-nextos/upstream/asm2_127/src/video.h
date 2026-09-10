#ifndef ASM2_VIDEO_H
#define ASM2_VIDEO_H

#include <SDL2/SDL.h>

struct asm2_video {
  SDL_Window *window;
  SDL_GLContext context;
  int width;
  int height;
  int force_opaque_alpha;
};

int asm2_video_init(struct asm2_video *video);
void asm2_video_swap(struct asm2_video *video);
void asm2_video_shutdown(struct asm2_video *video);

#endif
