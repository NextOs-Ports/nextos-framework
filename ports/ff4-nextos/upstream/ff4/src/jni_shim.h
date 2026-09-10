// jni_shim — JNIEnv falso para o libff4 (FF IV 3D base) chamar os "metodos
// estaticos do MainActivity" sem nenhuma JVM.
//
// O libff4 usa a ABI JNI de verdade: render/resume/pause/touch recebem
// (JNIEnv*, jobject, ...) e chamam FindClass + GetStaticMethodID + CallStatic*
// na classe com/square_enix/android_googleplay/FFIV_GP/MainActivity.
//
// Diferente do ff3 (que EMPURRA o pad no 7o arg de touch), o FF4 PUXA o estado:
//   getKeyEvent()  -> mascara de botoes (main.c seta via jni_set_keystate)
//   getResWidth/Height() -> resolucao (jni_set_screen)
//   getCurrentFrame(J) -> frame limiter
//   setFPS(I) -> alvo de fps do engine
#ifndef JNI_SHIM_H
#define JNI_SHIM_H

#include <stdint.h>

typedef struct jni_env jni_env_t;   // o que o libff4 ve como JNIEnv*
typedef void *jni_obj_t;

void jni_shim_init(void);
jni_env_t *jni_shim_env(void);
jni_obj_t jni_shim_activity(void);

// main.c empurra o estado que os upcalls do FF4 puxam.
void jni_set_keystate(int mask);   // getKeyEvent()
void jni_set_screen(int w, int h); // getResWidth()/getResHeight()
int  jni_get_back_mode(void);      // 0=off, 1, 2 (assignBackButton) — B-cancel do main.c
int  jni_get_fps(void);            // ultimo setFPS(I) do engine (default 30)
void jni_set_pad_poll(void (*cb)(void)); // callback: re-amostra o pad a cada tick logico

#endif
void jni_get_view(int *x, int *y, int *w, int *h);
