#pragma once
#include "jni.h"
#include "jni_internals.h"

/* [NextOS/forager] ver lang_Double.cpp */
class LangDouble : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }

    LangDouble(double v) : value(v) {}

    static jdouble doubleValue(JNIEnv *env, jobject obj, jclass clz);
    static jfloat  floatValue(JNIEnv *env, jobject obj, jclass clz);
    static jint    intValue(JNIEnv *env, jobject obj, jclass clz);

    double value;
};
