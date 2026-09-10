/* SPDX-License-Identifier: GPL-3.0-only */
/* NextOS original teaching contract. Only the JNI slots used below exist. */
#ifndef NEXTOS_TRAINING_DEMO_H
#define NEXTOS_TRAINING_DEMO_H
#include <stdint.h>
#define DEMO_WIDTH 160
#define DEMO_HEIGHT 120
#define DEMO_JNI_VERSION 0x00010006
typedef const struct demo_jni_table *DemoEnv;
typedef const struct demo_vm_table *DemoVM;
struct demo_jni_table {
    void *reserved[4];
    int32_t (*GetVersion)(DemoEnv *);
};
struct demo_vm_table {
    void *reserved[3];
    int32_t (*DestroyJavaVM)(DemoVM *);
    int32_t (*AttachCurrentThread)(DemoVM *, void **, void *);
    int32_t (*DetachCurrentThread)(DemoVM *);
    int32_t (*GetEnv)(DemoVM *, void **, int32_t);
    int32_t (*AttachCurrentThreadAsDaemon)(DemoVM *, void **, void *);
};
/* Native input bits owned by this demo, never copied from a commercial game. */
enum { DEMO_LEFT=1, DEMO_RIGHT=2, DEMO_UP=4, DEMO_DOWN=8 };
int32_t JNI_OnLoad(DemoVM *, void *);
int demo_create(int seed);
int demo_resume(void);
int demo_step(unsigned buttons, unsigned elapsed_ms);
int demo_render(uint32_t *rgba, unsigned count);
int demo_audio(int16_t *pcm, unsigned count);
int demo_pause(void);
int demo_save(void);
int demo_restore(int score);
int demo_destroy(void);
#endif
