/*
 * jni_shim.h -- fake JNI environment for the loaded Android game
 *
 * Provides a fake JavaVM and JNIEnv with stub vtables so that
 * JNI calls from the game library don't crash.
 */

#ifndef __JNI_SHIM_H__
#define __JNI_SHIM_H__

void jni_shim_init(void **out_vm, void **out_env);

// Set package name and OBB version (call before jni_shim_init)
void jni_shim_set_package(const char *package_name, int obb_version);

// Cria um handle de array JNI fake apontando p/ dados C (int32/float).
// Servido por GetArrayLength/GetIntArrayRegion/GetFloatArrayRegion.
void *jni_shim_make_array(const void *data, int len);

#endif
