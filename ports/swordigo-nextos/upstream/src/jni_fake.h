#ifndef SWORDIGO_JNI_FAKE_H
#define SWORDIGO_JNI_FAKE_H

typedef void (*JniTextChangedCallback)(void *env, void *obj, void *jstr);
typedef void (*JniTextFinishedCallback)(void *env, void *obj);

extern void *fake_env;
extern void *fake_vm;

void jni_init(void);
void *jni_make_object(const char *label);
void *jni_make_string(const char *utf);
void jni_configure_text_input(JniTextChangedCallback changed,
                              JniTextFinishedCallback finished);
void jni_update(void);

#endif
