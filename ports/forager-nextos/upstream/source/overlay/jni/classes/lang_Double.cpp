/* [NextOS/forager] java/lang/Double
 *
 * Ext_Call (o despachante de funcao de extensao do GameMaker) chama
 * RunnerJNILib.CallExtensionFunction(...) e, para descobrir o valor de retorno, faz
 * FindClass("java/lang/Double") + doubleValue(). Sem esta classe o FindClass devolvia
 * NULL, o valor virava `undefined` e o jogo estourava com
 * "unable to add undefined to a string" no Draw do WorldControl
 * (era o NOTCH_getSafeInsetRight, chamado todo frame).
 *
 * Tambem exponho valueOf()/doubleValue() porque sao os dois caminhos usuais de
 * boxing/unboxing.
 */
#include <vector>

#include "jni.h"
#include "jni_internals.h"
#include "lang_Double.h"

jdouble LangDouble::doubleValue(JNIEnv *env, jobject obj, jclass clz)
{
    LangDouble *self = (LangDouble *)obj;
    return self ? self->value : 0.0;
}

jfloat LangDouble::floatValue(JNIEnv *env, jobject obj, jclass clz)
{
    LangDouble *self = (LangDouble *)obj;
    return self ? (jfloat)self->value : 0.0f;
}

jint LangDouble::intValue(JNIEnv *env, jobject obj, jclass clz)
{
    LangDouble *self = (LangDouble *)obj;
    return self ? (jint)self->value : 0;
}

static const FieldId langDoubleClassFields[] = {
    {NULL},
};

static const ManagedMethod langDoubleClassMethods[] = {
    REGISTER_NONVIRTUAL(LangDouble, doubleValue, "()D"),
    REGISTER_NONVIRTUAL(LangDouble, floatValue,  "()F"),
    REGISTER_NONVIRTUAL(LangDouble, intValue,    "()I"),
    NULL
};

Class LangDouble::clazz = {
    .classpath = "java/lang/Double",
    .classname = "Double",
    .managed_methods = langDoubleClassMethods,
    .fields = langDoubleClassFields,
    .instance_size = sizeof(LangDouble)
};

static const int registered = ClassRegistry::register_class(LangDouble::clazz);
