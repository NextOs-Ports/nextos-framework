#ifndef __JNI_SHIM_H__
#define __JNI_SHIM_H__

void jni_shim_init(void **out_vm, void **out_env);

/* Cria um jstring falso que GetStringUTFChars resolve de volta para 'value'.
 * 'value' precisa permanecer valido (use literal ou strdup). */
void *jni_make_string(const char *value);

/* Caminho gravavel devolvido por Cocos2dxHelper.getCocos2dxWritablePath(). */
void jni_set_writable_path(const char *path);
void jni_set_assets_path(const char *path);

void* AAssetManager_fromJava(void* env, void* assetManager);

#endif
