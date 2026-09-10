/* [NextOS/forager] java/lang/Object
 *
 * O runner do GameMaker monta o array de argumentos de CallExtensionFunction com
 * NewObjectArray(len, FindClass("java/lang/Object"), NULL). Sem esta classe registrada,
 * o FindClass devolvia NULL e o NewObjectArray estourava (SIGSEGV no deref de
 * instance_size). Registrar uma classe minima resolve o caso na raiz, em vez de so
 * blindar o deref.
 *
 * instance_size = sizeof(void*): o array e sempre de REFERENCIAS (jobject), entao cada
 * elemento ocupa um ponteiro. Nao pode ser 0 — iface_NewObjectArray trata 0 como
 * "classe sem instancia" e devolve NULL.
 */
#include <vector>

#include "jni.h"
#include "jni_internals.h"

class LangObject : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }
};

Class LangObject::clazz = {
    .classpath = "java/lang/Object",
    .classname = "Object",
    .managed_methods = NULL,
    .fields = NULL,
    .instance_size = sizeof(void *)
};

static const int registered = ClassRegistry::register_class(LangObject::clazz);
