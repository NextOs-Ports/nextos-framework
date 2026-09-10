#ifndef HUNTDOWN_ANDROID_NDK_SHIM_H
#define HUNTDOWN_ANDROID_NDK_SHIM_H

#include <stddef.h>

typedef struct HdAndroidNdkSymbol {
  const char *name;
  void *address;
} HdAndroidNdkSymbol;

/* Android's UI thread owns a prepared native looper before Unity's JNI_OnLoad
 * runs.  Reproduce that ordering on the host so Unity 6 can retain it during
 * NativeLoader initialization. */
void hd_android_prepare_main_looper(void);

/* Correct-ABI implementations for the small Android NDK surface used by
 * Unity outside MediaNDK. */
const HdAndroidNdkSymbol *hd_android_ndk_symbols(size_t *count);

#endif
