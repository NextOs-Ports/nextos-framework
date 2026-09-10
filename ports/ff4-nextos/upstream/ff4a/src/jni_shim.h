/*
 * jni_shim.h -- JNIEnv/JavaVM falsos mínimos p/ o FF4.
 * Só os slots que a engine chama; loga upcalls-chave p/ recon (U3).
 */
#ifndef FF4A_JNI_SHIM_H
#define FF4A_JNI_SHIM_H

void jni_shim_init(void **out_vm, void **out_env);
/* objeto fake que passamos como "clazz"/"this" nos entry-points da engine. */
extern void *ff4a_fake_obj;

/* tela que a engine consulta via getResWidth/Height / getViewWidth/Height. */
void jni_set_screen(int w, int h);
/* estado de input (bitmask que getKeyEvent devolve) — main.c atualiza pelo pad. */
void jni_set_keystate(int mask);
/* modo do BACK Android anunciado pela engine (assignBackButton): 0=off,
 * 1->bit 0x8000, >=2->bit 0x4000. */
int jni_get_back_mode(void);
/* frame corrente (getCurrentFrame). */
void jni_set_frame(long f);

#endif
