/* jni_fake.h -- fake JNI environment for the KOTOR native libs.
 *
 * We stand in for the com.aspyr.kotor.KOTOR Activity: the only calls that
 * matter for boot are Java -> native (mountObb/mountPatchObb, which we invoke
 * directly with a fake JNIEnv whose GetStringUTFChars returns our OBB path).
 * A generic JNIEnv table covers any native -> Java callback with safe defaults.
 *
 * MIT license. See LICENSE.
 */
#ifndef __JNI_FAKE_H__
#define __JNI_FAKE_H__

extern void *fake_env;
extern void *fake_vm;

void jni_init(void);

/* build a fake jstring backed by a C string (for GetStringUTFChars). */
void *jni_make_string(const char *utf);
void *jni_make_string_array(int n, const char **strs);

#endif
