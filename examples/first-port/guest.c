/* SPDX-License-Identifier: GPL-3.0-only */
/* Original NextOS training game: no engine, assets or proprietary code. */
#include "demo.h"
extern int __android_log_write(int, const char *, const char *);
extern int *__errno(void);
static int initialized, loaded, state, x, y, score, remainder;
static unsigned tone_left, phase;
__attribute__((constructor)) static void initialize(void) {
    initialized++;
    __android_log_write(4, "NextOS training", "constructor");
}
int32_t JNI_OnLoad(DemoVM *vm, void *reserved) {
    DemoEnv *env = 0;
    if (initialized != 1 || reserved || !vm || !*vm ||
        (*vm)->GetEnv(vm, (void **)&env, DEMO_JNI_VERSION) != 0 ||
        !env || !*env || (*env)->GetVersion(env) != DEMO_JNI_VERSION)
        return -1;
    loaded=1;
    __android_log_write(4, "NextOS training", "JNI_OnLoad after constructor");
    return DEMO_JNI_VERSION;
}
int demo_create(int seed) {
    if (!loaded || state || seed != 7) { *__errno()=22; return -1; }
    x=20; y=60; score=0; remainder=0; state=1;
    tone_left=phase=0;
    return 0;
}
int demo_resume(void) { if(state!=1) return -1; state=2; return 0; }
int demo_step(unsigned buttons, unsigned elapsed_ms) {
    int moves;
    if(state!=2 || elapsed_ms>250 || (buttons & ~15u)) return -1;
    remainder+=(int)elapsed_ms; moves=remainder/16; remainder%=16;
    if(buttons & DEMO_LEFT) x-=moves;
    if(buttons & DEMO_RIGHT) x+=moves;
    if(buttons & DEMO_UP) y-=moves;
    if(buttons & DEMO_DOWN) y+=moves;
    if(x<3) x=3;
    if(x>156) x=156;
    if(y<3) y=3;
    if(y>116) y=116;
    if(x>=117 && x<=123 && y>=57 && y<=63) {
        if(score<1000000) score++;
        x=20; tone_left=4800;
    }
    return 0;
}
int demo_render(uint32_t *rgba, unsigned count) {
    unsigned i;
    if(state!=2 || !rgba || count!=DEMO_WIDTH*DEMO_HEIGHT) return -1;
    for(i=0;i<count;i++) {
        int px=(int)(i%DEMO_WIDTH), py=(int)(i/DEMO_WIDTH);
        uint32_t color=0xff302018u;
        if(px>=117 && px<=123 && py>=57 && py<=63) color=0xff40c0ffu;
        if(px>=x-2 && px<=x+2 && py>=y-2 && py<=y+2) color=0xffffc040u;
        if(py<4 && px<(score%16)*10) color=0xff40ff60u;
        rgba[i]=color;
    }
    return 0;
}
int demo_audio(int16_t *pcm, unsigned count) {
    unsigned i;
    if(state!=2 || !pcm || count>4800) return -1;
    for(i=0;i<count;i++) {
        pcm[i]=tone_left ? (phase%100<50 ? 4000 : -4000) : 0;
        phase++;
        if(tone_left) tone_left--;
    }
    return 0;
}
int demo_pause(void) { if(state!=2) return -1; state=1; return 0; }
int demo_save(void) { return state==1 ? score : -1; }
int demo_restore(int value) {
    if(state!=1 || value<0 || value>1000000) return -1;
    score=value; return 0;
}
int demo_destroy(void) { if(state!=1) return -1; state=0; return 0; }
