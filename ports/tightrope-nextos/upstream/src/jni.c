/*
 * jni.c -- the JNIEnv the Android objects talk to.
 *
 * The vtable is built by index rather than by naming the fields of
 * JNINativeInterface: the port has to match the real slot numbers exactly
 * because libpairipcore calls through them by offset (its ExecuteProgram loads
 * [env + 1336], which is slot 167, NewStringUTF), and an index table makes that
 * checkable against the disassembly.
 *
 * Objects are small tagged structs, not opaque cookies, so GetObjectClass and
 * IsInstanceOf can tell a KeyEvent from a MotionEvent from a byte[] -- SDL
 * needs that distinction and so does the PairIP VM, which passes byte arrays.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "nx_elf.h"
#include "tr.h"
#include "jni_slots.h"

int tr_trace_jni = 0;

#define JT(...) do { if (tr_trace_jni) nx_log("jni: " __VA_ARGS__); } while (0)

/* ------------------------------------------------------------- object model */

typedef enum {
    O_CLASS, O_STRING, O_BYTEARRAY, O_OBJARRAY, O_OBJECT, O_THROWABLE,
    O_DIRECTBUF,
} otype;

typedef struct jobj {
    otype type;
    const char *cls;         /* class name for O_OBJECT / O_CLASS */
    char *str;               /* O_STRING payload (UTF-8, owned) */
    void *data;              /* array payload */
    int len;
    struct jobj **elems;     /* O_OBJARRAY */
    int64_t prim;            /* payload of a boxed primitive */
    int boxed;
} jobj;

static jobj *new_obj(otype t, const char *cls)
{
    jobj *o = calloc(1, sizeof *o);
    o->type = t;
    o->cls = cls;
    return o;
}

static jobj *mk_class(const char *name)
{
    /* Classes are interned so IsSameObject and the method tables work. */
    static jobj *cache[256];
    static const char *names[256];
    static int n;
    for (int i = 0; i < n; i++)
        if (strcmp(names[i], name) == 0)
            return cache[i];
    if (n == 256)
        nx_die("class cache full (%s)", name);
    names[n] = strdup(name);
    cache[n] = new_obj(O_CLASS, names[n]);
    return cache[n++];
}

static jobj *mk_string(const char *s)
{
    jobj *o = new_obj(O_STRING, "java/lang/String");
    o->str = strdup(s ? s : "");
    o->len = (int)strlen(o->str);
    return o;
}

static jobj *mk_object(const char *cls)
{
    return new_obj(O_OBJECT, cls);
}

static jobj *mk_file(const char *path)
{
    jobj *o = mk_object("java/io/File");
    o->str = strdup(path ? path : "");
    o->len = (int)strlen(o->str);
    return o;
}

/* Objects published to the Android input surface SDL enumerates. */
static jobj *input_device;
static jobj *touch_device;
static jobj *motion_range_list;
static jobj *touch_motion_range_list;
static jobj *motion_range_iterator;
static jobj *motion_ranges[8];
static jobj *key_event_object;
static jobj *motion_event_object;
static jobj *preferences_object;
static jobj *preferences_editor;
static char input_device_name[128] = "NextOS Gamepad";
static char input_device_descriptor[160] = "nextos-native-gamepad";
static int input_device_vendor;
static int input_device_product;
static struct {
    int action;
    int keycode;
    int source;
    int device_id;
    int meta_state;
    int repeat;
    int scancode;
    int flags;
    int unicode;
    int64_t event_time;
    int64_t down_time[256];
} key_event;
typedef struct {
    int action;
    int source;
    int device_id;
    int meta_state;
    int button_state;
    int flags;
    int64_t event_time;
    int64_t down_time;
    float axis[48];
} motion_payload;
static motion_payload motion_event;
#define MOTION_CLONE_COUNT 64
static jobj *motion_clones[MOTION_CLONE_COUNT];
static motion_payload motion_clone_data[MOTION_CLONE_COUNT];
static unsigned motion_clone_next;

static const int motion_axis_ids[8] = {
    0, 1, 11, 14, 17, 18, 15, 16,
};

static motion_payload *motion_from_object(jobj *object)
{
    return object && object->data
        ? (motion_payload *)object->data : &motion_event;
}

static int64_t monotonic_millis(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}











/* ----------------------------------------------------------- method registry */

/* Method IDs are indices into a flat table so a handler can be looked up in
 * one step and an unknown method still returns a usable ID (returning NULL
 * from GetMethodID makes SDL abort long before we learn what it wanted). */
typedef struct {
    const char *cls, *name, *sig;
    void *handler;          /* tr_jni_handler, or NULL for "safe default" */
} jmethod;

#define MAX_METHODS 1024
static jmethod methods[MAX_METHODS];
static int method_n;

typedef struct jctx {
    void *env;
    jobj *self;
    va_list *ap;
    const uint64_t *args;
    unsigned arg_index;
    const jmethod *m;
} jctx;
typedef int64_t (*tr_jni_handler)(jctx *);

/* JNI's A calls carry a jvalue[] instead of a va_list.  Both representations
 * use one eight-byte slot per argument on this aarch64 target; keeping the
 * readers here lets the same platform handler serve Call*Method, *V and *A. */
static jobj *jarg_obj(jctx *c)
{
    if (c->args)
        return (jobj *)(uintptr_t)c->args[c->arg_index++];
    return va_arg(*c->ap, jobj *);
}

static int32_t jarg_int(jctx *c)
{
    if (c->args)
        return (int32_t)c->args[c->arg_index++];
    return va_arg(*c->ap, int32_t);
}

static int64_t jarg_long(jctx *c)
{
    if (c->args)
        return (int64_t)c->args[c->arg_index++];
    return va_arg(*c->ap, int64_t);
}

static float jarg_float(jctx *c)
{
    float value;
    if (c->args) {
        uint32_t bits = (uint32_t)c->args[c->arg_index++];
        memcpy(&value, &bits, sizeof value);
        return value;
    }
    return (float)va_arg(*c->ap, double);
}

static void *method_id(const char *cls, const char *name, const char *sig)
{
    for (int i = 0; i < method_n; i++)
        if (strcmp(methods[i].name, name) == 0 &&
            strcmp(methods[i].sig, sig) == 0 &&
            (!cls || !methods[i].cls || strcmp(methods[i].cls, cls) == 0))
            return (void *)(uintptr_t)(i + 1);
    if (method_n == MAX_METHODS)
        nx_die("method table full");
    methods[method_n].cls = cls ? strdup(cls) : NULL;
    methods[method_n].name = strdup(name);
    methods[method_n].sig = strdup(sig);
    methods[method_n].handler = NULL;
    JT("new method id %d %s.%s%s", method_n + 1, cls ? cls : "?", name, sig);
    return (void *)(uintptr_t)(++method_n);
}

static const jmethod *by_id(void *mid)
{
    uintptr_t i = (uintptr_t)mid;
    return (i >= 1 && i <= (uintptr_t)method_n) ? &methods[i - 1] : NULL;
}

void tr_jni_bind(const char *cls, const char *name, const char *sig, void *fn)
{
    void *id = method_id(cls, name, sig);
    methods[(uintptr_t)id - 1].handler = fn;
}

/* --------------------------------------------------------------- registry of
 * natives that the loaded objects register with us */

typedef struct { char cls[128]; char name[128]; char sig[192]; void *fn; } jnative;
#define MAX_NATIVES 512
static jnative natives[MAX_NATIVES];
static int natives_n;

void *tr_jni_native(const char *cls, const char *name)
{
    for (int i = 0; i < natives_n; i++)
        if (strcmp(natives[i].name, name) == 0 &&
            (!cls || strcmp(natives[i].cls, cls) == 0))
            return natives[i].fn;

    /* SDL does not call RegisterNatives: its Android entry points are ordinary
     * exported symbols with the Java_<class>_<method> spelling, so a name that
     * is not in the registry is still resolvable by mangling it. */
    if (!cls)
        return NULL;
    char sym[320];
    size_t k = (size_t)snprintf(sym, sizeof sym, "Java_%s_%s", cls, name);
    if (k >= sizeof sym)
        return NULL;
    for (char *p = sym; *p; p++)
        if (*p == '/')
            *p = '_';
    return nx_lookup(sym);
}







/* ------------------------------------------------------------------ vtable */

static void *vt[JNI_SLOT_COUNT];
static void *env_ptr = vt;          /* JNIEnv* is a pointer to the vtable ptr */
static void *jvm_vt[8];
static void *jvm_ptr = jvm_vt;

void *tr_jni_env(void) { return &env_ptr; }
void *tr_jni_vm(void) { return &jvm_ptr; }



static int64_t monotonic_nanos(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
}






/* --- basics ------------------------------------------------------------- */

static int32_t j_GetVersion(void *e) { (void)e; return 0x00010006; }

static jobj *j_FindClass(void *e, const char *name)
{
    (void)e;
    JT("FindClass %s", name);
    return mk_class(name);
}

static jobj *j_GetObjectClass(void *e, jobj *o)
{
    (void)e;
    return mk_class(o && o->cls ? o->cls : "java/lang/Object");
}

static uint8_t j_IsInstanceOf(void *e, jobj *o, jobj *c)
{
    (void)e;
    if (!o || !c || !c->cls)
        return 0;
    if (strcmp(c->cls, "java/lang/Object") == 0)
        return 1;
    if (strcmp(c->cls, "android/view/InputEvent") == 0 && o->cls &&
        (strcmp(o->cls, "android/view/KeyEvent") == 0 ||
         strcmp(o->cls, "android/view/MotionEvent") == 0))
        return 1;
    return o->cls && strcmp(o->cls, c->cls) == 0;
}

static uint8_t j_IsAssignableFrom(void *e, jobj *a, jobj *b)
{
    (void)e;
    return a && b && a->cls && b->cls && strcmp(a->cls, b->cls) == 0;
}

static jobj *j_GetSuperclass(void *e, jobj *c) { (void)e; (void)c; return mk_class("java/lang/Object"); }

static void *j_NewGlobalRef(void *e, void *o) { (void)e; return o; }
static void *j_NewLocalRef(void *e, void *o) { (void)e; return o; }
static void *j_NewWeakGlobalRef(void *e, void *o) { (void)e; return o; }
static void j_DeleteRef(void *e, void *o) { (void)e; (void)o; }
static uint8_t j_IsSameObject(void *e, void *a, void *b) { (void)e; return a == b; }
static int32_t j_PushLocalFrame(void *e, int32_t n) { (void)e; (void)n; return 0; }
static void *j_PopLocalFrame(void *e, void *r) { (void)e; return r; }
static int32_t j_EnsureLocalCapacity(void *e, int32_t n) { (void)e; (void)n; return 0; }

static jobj *pending_exception;
static void *j_ExceptionOccurred(void *e) { (void)e; return pending_exception; }
static void j_ExceptionDescribe(void *e) { (void)e; }
static void j_ExceptionClear(void *e) { (void)e; pending_exception = NULL; }
static uint8_t j_ExceptionCheck(void *e) { (void)e; return pending_exception != NULL; }
static int32_t j_Throw(void *e, jobj *t) { (void)e; pending_exception = t; return 0; }
static int32_t j_ThrowNew(void *e, jobj *c, const char *m)
{
    (void)e;
    nx_log("jni: ThrowNew %s: %s", c && c->cls ? c->cls : "?", m ? m : "");
    pending_exception = new_obj(O_THROWABLE, c ? c->cls : "java/lang/Exception");
    return 0;
}
static void j_FatalError(void *e, const char *m) { (void)e; nx_die("JNI FatalError: %s", m); }

static int32_t j_GetJavaVM(void *e, void **vm) { (void)e; *vm = tr_jni_vm(); return 0; }
static int32_t j_MonitorEnter(void *e, void *o) { (void)e; (void)o; return 0; }
static int32_t j_MonitorExit(void *e, void *o) { (void)e; (void)o; return 0; }

/* --- strings ----------------------------------------------------------- */

static jobj *j_NewStringUTF(void *e, const char *s) { (void)e; return mk_string(s); }
static const char *j_GetStringUTFChars(void *e, jobj *s, uint8_t *copy)
{
    (void)e;
    if (copy) *copy = 0;
    return s && s->str ? s->str : "";
}
static void j_ReleaseStringUTFChars(void *e, jobj *s, const char *c) { (void)e; (void)s; (void)c; }
static int32_t j_GetStringUTFLength(void *e, jobj *s) { (void)e; return s ? s->len : 0; }
static int32_t j_GetStringLength(void *e, jobj *s) { (void)e; return s ? s->len : 0; }
static jobj *j_NewString(void *e, const uint16_t *u, int32_t n)
{
    (void)e;
    char *b = malloc((size_t)n + 1);
    for (int32_t i = 0; i < n; i++)
        b[i] = (char)(u[i] < 128 ? u[i] : '?');
    b[n] = 0;
    jobj *o = mk_string(b);
    free(b);
    return o;
}
static const uint16_t *j_GetStringChars(void *e, jobj *s, uint8_t *copy)
{
    (void)e;
    if (copy) *copy = 1;
    int n = s ? s->len : 0;
    uint16_t *u = calloc((size_t)n + 1, 2);
    for (int i = 0; i < n; i++)
        u[i] = (uint8_t)s->str[i];
    return u;
}
static void j_ReleaseStringChars(void *e, jobj *s, const uint16_t *u) { (void)e; (void)s; free((void *)u); }
static void j_GetStringUTFRegion(void *e, jobj *s, int32_t off, int32_t len, char *out)
{
    (void)e;
    if (s && s->str)
        memcpy(out, s->str + off, (size_t)len);
}

/* --- arrays ------------------------------------------------------------ */

static int32_t j_GetArrayLength(void *e, jobj *a) { (void)e; return a ? a->len : 0; }

static jobj *j_NewByteArray(void *e, int32_t n)
{
    (void)e;
    jobj *o = new_obj(O_BYTEARRAY, "[B");
    o->len = n;
    o->data = calloc((size_t)(n > 0 ? n : 1), 1);
    return o;
}

static jobj *j_NewIntArray(void *e, int32_t n)
{
    (void)e;
    jobj *o = new_obj(O_BYTEARRAY, "[I");
    o->len = n;
    o->data = calloc((size_t)(n > 0 ? n : 1), sizeof(int32_t));
    return o;
}

static jobj *mk_int_array(const int32_t *values, int32_t n)
{
    jobj *o = j_NewIntArray(NULL, n);
    if (values && n > 0)
        memcpy(o->data, values, (size_t)n * sizeof *values);
    return o;
}
static void *j_GetByteArrayElements(void *e, jobj *a, uint8_t *copy)
{
    (void)e;
    if (copy) *copy = 0;
    return a ? a->data : NULL;
}
static void j_ReleaseArrayElements(void *e, jobj *a, void *p, int32_t mode)
{
    (void)e; (void)a; (void)p; (void)mode;
}
static void j_GetByteArrayRegion(void *e, jobj *a, int32_t s, int32_t n, void *buf)
{
    (void)e;
    if (a && a->data)
        memcpy(buf, (char *)a->data + s, (size_t)n);
}
static void j_SetByteArrayRegion(void *e, jobj *a, int32_t s, int32_t n, const void *buf)
{
    (void)e;
    if (a && a->data)
        memcpy((char *)a->data + s, buf, (size_t)n);
}

static void j_GetIntArrayRegion(void *e, jobj *a, int32_t s, int32_t n, void *buf)
{
    (void)e;
    if (a && a->data)
        memcpy(buf, (int32_t *)a->data + s, (size_t)n * sizeof(int32_t));
}

static void j_SetIntArrayRegion(void *e, jobj *a, int32_t s, int32_t n,
                                const void *buf)
{
    (void)e;
    if (a && a->data)
        memcpy((int32_t *)a->data + s, buf, (size_t)n * sizeof(int32_t));
}
static jobj *j_NewObjectArray(void *e, int32_t n, jobj *cls, jobj *init)
{
    (void)e; (void)cls;
    jobj *o = new_obj(O_OBJARRAY, "[Ljava/lang/Object;");
    o->len = n;
    o->elems = calloc((size_t)(n > 0 ? n : 1), sizeof *o->elems);
    for (int32_t i = 0; i < n; i++)
        o->elems[i] = init;
    return o;
}
static jobj *j_GetObjectArrayElement(void *e, jobj *a, int32_t i)
{
    (void)e;
    return (a && a->elems && i >= 0 && i < a->len) ? a->elems[i] : NULL;
}
static void j_SetObjectArrayElement(void *e, jobj *a, int32_t i, jobj *v)
{
    (void)e;
    if (a && a->elems && i >= 0 && i < a->len)
        a->elems[i] = v;
}
static void *j_GetPrimitiveArrayCritical(void *e, jobj *a, uint8_t *copy)
{
    return j_GetByteArrayElements(e, a, copy);
}
static void j_ReleasePrimitiveArrayCritical(void *e, jobj *a, void *p, int32_t m)
{
    (void)e; (void)a; (void)p; (void)m;
}
static jobj *j_NewDirectByteBuffer(void *e, void *addr, int64_t cap)
{
    (void)e;
    jobj *o = new_obj(O_DIRECTBUF, "java/nio/ByteBuffer");
    o->data = addr;
    o->len = (int)cap;
    return o;
}
static void *j_GetDirectBufferAddress(void *e, jobj *b) { (void)e; return b ? b->data : NULL; }
static int64_t j_GetDirectBufferCapacity(void *e, jobj *b) { (void)e; return b ? b->len : -1; }
static int32_t j_GetObjectRefType(void *e, void *o) { (void)e; return o ? 2 : 0; }

/* --- calls ------------------------------------------------------------- */

static int is_unbox(const char *name)
{
    static const char *const n[] = { "longValue", "intValue", "shortValue",
                                     "byteValue", "charValue", "booleanValue",
                                     "floatValue", "doubleValue" };
    for (size_t i = 0; i < sizeof n / sizeof *n; i++)
        if (strcmp(name, n[i]) == 0)
            return 1;
    return 0;
}

/* --- Android input devices, KeyEvent and MotionEvent ------------------ */

static int64_t jfloat_result(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static int64_t j_InputDevice_getDeviceIds(jctx *c)
{
    (void)c;
    /* Real Android enumerates its built-in touchscreen as an InputDevice too.
     * PF2 uses the newer Input System, which must see that device during its
     * startup scan before later touchscreen MotionEvents can become Touches. */
    const int32_t ids[] = { 0, 1 };
    static int logged;
    if (!logged++)
        nx_log("jni: publishing Android touchscreen id=0 and gamepad id=1 "
               "(%s %04x:%04x)",
               input_device_name, input_device_vendor & 0xffff,
               input_device_product & 0xffff);
    return (int64_t)(uintptr_t)mk_int_array(ids, 2);
}

static int64_t j_InputDevice_getDevice(jctx *c)
{
    int id = jarg_int(c);
    return (int64_t)(uintptr_t)(id == 0 ? touch_device :
                                id == 1 ? input_device : NULL);
}

static int64_t j_InputEvent_getDevice(jctx *c)
{
    if (c->self && c->self->cls &&
        strcmp(c->self->cls, "android/view/MotionEvent") == 0 &&
        motion_from_object(c->self)->device_id == 0) {
        JT("touch getDevice -> %p (id=0 sources=%#x)",
           (void *)touch_device, 0x00001002);
        return (int64_t)(uintptr_t)touch_device;
    }
    return (int64_t)(uintptr_t)input_device;
}

static int64_t j_InputDevice_getString(jctx *c)
{
    if (c->self == touch_device)
        return (int64_t)(uintptr_t)mk_string(
            strcmp(c->m->name, "getDescriptor") == 0
                ? "nextos:touch:0" : "NextOS Touchscreen");
    const char *value = strcmp(c->m->name, "getDescriptor") == 0
        ? input_device_descriptor : input_device_name;
    return (int64_t)(uintptr_t)mk_string(value);
}

static int64_t j_InputDevice_getInt(jctx *c)
{
    if (c->self == touch_device) {
        if (strcmp(c->m->name, "getSources") == 0) return 0x00001002;
        if (strcmp(c->m->name, "getId") == 0) return 0;
        return 0;
    }
    if (strcmp(c->m->name, "getVendorId") == 0) return input_device_vendor;
    if (strcmp(c->m->name, "getProductId") == 0) return input_device_product;
    if (strcmp(c->m->name, "getSources") == 0) return 0x01000611;
    if (strcmp(c->m->name, "getId") == 0) return 1;
    if (strcmp(c->m->name, "getControllerNumber") == 0) return 1;
    return 0; /* KEYBOARD_TYPE_NONE */
}

static int64_t j_InputDevice_supportsSource(jctx *c)
{
    int source = jarg_int(c);
    int available = c->self == touch_device ? 0x00001002 : 0x01000611;
    return (available & source) == source;
}

static int64_t j_InputDevice_getMotionRanges(jctx *c)
{
    return (int64_t)(uintptr_t)(c->self == touch_device
                                    ? touch_motion_range_list
                                    : motion_range_list);
}

static int64_t j_InputDevice_getMotionRange(jctx *c)
{
    int axis = jarg_int(c);
    if (c->self == touch_device)
        return 0;
    for (size_t i = 0; i < sizeof motion_axis_ids /
                            sizeof *motion_axis_ids; i++)
        if (motion_axis_ids[i] == axis)
            return (int64_t)(uintptr_t)motion_ranges[i];
    return 0;
}

static int64_t j_List_size(jctx *c)
{
    return c->self ? c->self->len : 0;
}

static int64_t j_List_get(jctx *c)
{
    int index = jarg_int(c);
    if (!c->self || !c->self->elems || index < 0 || index >= c->self->len)
        return 0;
    return (int64_t)(uintptr_t)c->self->elems[index];
}

static int64_t j_List_iterator(jctx *c)
{
    motion_range_iterator->elems = c->self ? c->self->elems : NULL;
    motion_range_iterator->len = c->self ? c->self->len : 0;
    motion_range_iterator->prim = 0;
    return (int64_t)(uintptr_t)motion_range_iterator;
}

static int64_t j_Iterator_hasNext(jctx *c)
{
    return c->self && c->self->prim < c->self->len;
}

static int64_t j_Iterator_next(jctx *c)
{
    if (!c->self || !c->self->elems ||
        c->self->prim < 0 || c->self->prim >= c->self->len)
        return 0;
    return (int64_t)(uintptr_t)c->self->elems[c->self->prim++];
}

static int64_t j_MotionRange_getInt(jctx *c)
{
    if (!c->self)
        return 0;
    if (strcmp(c->m->name, "getAxis") == 0)
        return c->self->prim;
    return 0x01000010; /* SOURCE_JOYSTICK */
}

static int64_t j_MotionRange_getFloat(jctx *c)
{
    int axis = c->self ? (int)c->self->prim : -1;
    float value = 0.0f;
    if (strcmp(c->m->name, "getMin") == 0)
        value = axis == 17 || axis == 18 ? 0.0f : -1.0f;
    else if (strcmp(c->m->name, "getMax") == 0)
        value = 1.0f;
    else if (strcmp(c->m->name, "getFlat") == 0)
        value = 0.05f;
    else if (strcmp(c->m->name, "getResolution") == 0)
        value = 1.0f / 32767.0f;
    return jfloat_result(value);
}

static int64_t j_KeyEvent_getInt(jctx *c)
{
    if (strcmp(c->m->name, "getAction") == 0) return key_event.action;
    if (strcmp(c->m->name, "getKeyCode") == 0) return key_event.keycode;
    if (strcmp(c->m->name, "getSource") == 0) return key_event.source;
    if (strcmp(c->m->name, "getDeviceId") == 0) return key_event.device_id;
    if (strcmp(c->m->name, "getMetaState") == 0) return key_event.meta_state;
    if (strcmp(c->m->name, "getRepeatCount") == 0) return key_event.repeat;
    if (strcmp(c->m->name, "getScanCode") == 0) return key_event.scancode;
    if (strcmp(c->m->name, "getFlags") == 0) return key_event.flags;
    if (strcmp(c->m->name, "getUnicodeChar") == 0) return key_event.unicode;
    return 0;
}

static int64_t j_KeyEvent_getLong(jctx *c)
{
    if (strcmp(c->m->name, "getDownTime") == 0)
        return key_event.down_time[key_event.keycode];
    return key_event.event_time;
}

static int64_t j_KeyEvent_isSystem(jctx *c)
{
    (void)c;
    return 0;
}

static int64_t j_MotionEvent_getInt(jctx *c)
{
    const char *name = c->m->name;
    if (c->self && c->self->cls &&
        strcmp(c->self->cls, "android/view/KeyEvent") == 0)
        return j_KeyEvent_getInt(c);
    motion_payload *event = motion_from_object(c->self);
    if (strcmp(name, "getPointerId") == 0 ||
        strcmp(name, "findPointerIndex") == 0 ||
        strcmp(name, "getToolType") == 0)
        (void)jarg_int(c);
    if (strcmp(name, "getAction") == 0 ||
        strcmp(name, "getActionMasked") == 0) {
        int value = strcmp(name, "getActionMasked") == 0
            ? event->action & 0xff : event->action;
        if (event->source == 0x00001002)
            JT("touch %s -> %d", name, value);
        return value;
    }
    if (strcmp(name, "getSource") == 0) {
        if (event->source == 0x00001002)
            JT("touch getSource -> %#x", event->source);
        return event->source;
    }
    if (strcmp(name, "getDeviceId") == 0) {
        if (event->source == 0x00001002)
            JT("touch getDeviceId -> %d", event->device_id);
        return event->device_id;
    }
    if (strcmp(name, "getMetaState") == 0) return event->meta_state;
    if (strcmp(name, "getButtonState") == 0) return event->button_state;
    if (strcmp(name, "getFlags") == 0) return event->flags;
    if (strcmp(name, "getPointerCount") == 0) return 1;
    if (strcmp(name, "getPointerId") == 0) return 0;
    if (strcmp(name, "findPointerIndex") == 0) return 0;
    if (strcmp(name, "getToolType") == 0)
        return event->source == 0x00001002 ? 1 : 0; /* TOOL_TYPE_FINGER */
    return 0; /* history/action index, edge flags, tool type, display id */
}

static int64_t j_MotionEvent_getLong(jctx *c)
{
    if (c->self && c->self->cls &&
        strcmp(c->self->cls, "android/view/KeyEvent") == 0)
        return j_KeyEvent_getLong(c);
    motion_payload *event = motion_from_object(c->self);
    if (c->m->sig[1] == 'I')
        (void)jarg_int(c);
    int64_t value = strcmp(c->m->name, "getDownTime") == 0
        ? event->down_time : event->event_time;
    if (event->source == 0x00001002)
        JT("touch %s -> %lld", c->m->name, (long long)value);
    return value;
}

static int64_t j_MotionEvent_getFloat(jctx *c)
{
    const char *name = c->m->name;
    motion_payload *event = motion_from_object(c->self);
    int axis = -1;
    if (strcmp(name, "getAxisValue") == 0 ||
        strcmp(name, "getHistoricalAxisValue") == 0) {
        axis = jarg_int(c);
    } else if (strcmp(name, "getX") == 0 ||
               strcmp(name, "getHistoricalX") == 0 ||
               strcmp(name, "getRawX") == 0) {
        axis = 0;
    } else if (strcmp(name, "getY") == 0 ||
               strcmp(name, "getHistoricalY") == 0 ||
               strcmp(name, "getRawY") == 0) {
        axis = 1;
    }
    float value;
    if (strcmp(name, "getPressure") == 0)
        value = event->source == 0x00001002 ? 1.0f : 0.0f;
    else if (strcmp(name, "getSize") == 0)
        value = event->source == 0x00001002 ? 1.0f : 0.0f;
    else if (strcmp(name, "getTouchMajor") == 0 ||
             strcmp(name, "getTouchMinor") == 0 ||
             strcmp(name, "getToolMajor") == 0 ||
             strcmp(name, "getToolMinor") == 0)
        value = event->source == 0x00001002 ? 8.0f : 0.0f;
    else
        value = axis >= 0 && axis < (int)(sizeof event->axis /
                                          sizeof *event->axis)
                  ? event->axis[axis] : 0.0f;
    if (event->source == 0x00001002)
        JT("touch %s axis=%d -> %.3f", name, axis, value);
    return jfloat_result(value);
}

static int64_t j_MotionEvent_isFromSource(jctx *c)
{
    motion_payload *event = motion_from_object(c->self);
    int source = jarg_int(c);
    return (event->source & source) == source;
}

static int64_t j_MotionEvent_obtain(jctx *c)
{
    jobj *source = jarg_obj(c);
    unsigned slot = motion_clone_next++ % MOTION_CLONE_COUNT;
    if (!motion_clones[slot])
        motion_clones[slot] = mk_object("android/view/MotionEvent");
    motion_clone_data[slot] = *motion_from_object(source);
    motion_clones[slot]->data = &motion_clone_data[slot];
    if (motion_clone_data[slot].source == 0x00001002)
        JT("touch obtain clone=%u action=%d xy=%.1f,%.1f "
           "down=%lld event=%lld", slot, motion_clone_data[slot].action,
           motion_clone_data[slot].axis[0], motion_clone_data[slot].axis[1],
           (long long)motion_clone_data[slot].down_time,
           (long long)motion_clone_data[slot].event_time);
    return (int64_t)(uintptr_t)motion_clones[slot];
}

static int64_t j_MotionEvent_recycle(jctx *c)
{
    (void)c;
    return 0;
}

/* java.io.File.  The PairIP startup program constructs one and asks for its
 * length before it does anything else, and a zero length is not a neutral
 * answer: it is exactly what a truncated or missing install looks like, so the
 * program takes its report-and-bail branch instead of the branch that does the
 * work.  These are ordinary platform APIs, not PairIP internals. */
static int64_t j_File_length(jctx *c)
{
    const char *p = c->self ? c->self->str : NULL;
    struct stat st;
    if (!p || stat(p, &st) != 0) {
        JT("File.length(\"%s\") -> 0 (no such file)", p ? p : "(null)");
        return 0;
    }
    /* procfs reports st_size 0 for files that do have content.  The caller
     * here sizes a buffer from this answer, so give it the real number of
     * bytes the file yields rather than the zero stat() reports. */
    if (st.st_size == 0 && S_ISREG(st.st_mode)) {
        FILE *f = fopen(p, "rb");
        if (f) {
            char scratch[4096];
            size_t total = 0, got;
            while ((got = fread(scratch, 1, sizeof scratch, f)) > 0)
                total += got;
            fclose(f);
            if (total) {
                JT("File.length(\"%s\") -> %zu (read, stat said 0)", p, total);
                return (int64_t)total;
            }
        }
    }
    JT("File.length(\"%s\") -> %lld", p, (long long)st.st_size);
    return (int64_t)st.st_size;
}

static int64_t j_File_exists(jctx *c)
{
    const char *p = c->self ? c->self->str : NULL;
    struct stat st;
    int64_t r = p && stat(p, &st) == 0;
    JT("File.exists(\"%s\") -> %lld", p ? p : "(null)", (long long)r);
    return r;
}

static int64_t j_File_getPath(jctx *c)
{
    return (int64_t)(uintptr_t)mk_string(c->self ? c->self->str : NULL);
}

static int64_t j_File_getParent(jctx *c)
{
    const char *path = c->self && c->self->str ? c->self->str : "";
    char buf[1200];
    snprintf(buf, sizeof buf, "%s", path);
    char *slash = strrchr(buf, '/');
    if (!slash)
        buf[0] = 0;
    else if (slash == buf)
        slash[1] = 0;
    else
        *slash = 0;
    return (int64_t)(uintptr_t)mk_string(buf);
}

static int64_t j_File_getParentFile(jctx *c)
{
    jobj *s = (jobj *)(uintptr_t)j_File_getParent(c);
    return (int64_t)(uintptr_t)mk_file(s ? s->str : "");
}

static int64_t j_File_isDirectory(jctx *c)
{
    struct stat st;
    const char *p = c->self && c->self->str ? c->self->str : "";
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int64_t j_File_mkdirs(jctx *c)
{
    const char *p = c->self && c->self->str ? c->self->str : "";
    if (!*p)
        return 0;
    if (mkdir(p, 0755) == 0 || errno == EEXIST)
        return 1;
    return 0;
}

static int64_t j_Context_getFilesDir(jctx *c)
{
    (void)c;
    /* Lime treats this as its application directory and resolves every
     * relative asset against it -- manifest/default.json first of all.  On
     * Android that directory is where the packaged assets are visible, so it
     * is the extracted asset root here.  Saves land in the same tree, which is
     * what the Android build does with its files directory. */
    return (int64_t)(uintptr_t)mk_file(tr_datadir);
}

static int64_t j_Context_getPackageCodePath(jctx *c)
{
    (void)c;
    /* Lime asks for the APK path and hands it to SDL's asset layer, which
     * only ever appends "assets/<name>" to it.  The extracted tree below
     * <gamedir>/assets reproduces that layout exactly. */
    return (int64_t)(uintptr_t)mk_string(tr_gamedir);
}

static int64_t j_Context_getPackageName(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)mk_string("com.adventureislands.tightropetheatre");
}

static int64_t j_Context_getAssets(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)mk_object("android/content/res/AssetManager");
}

/* --- assets ------------------------------------------------------------ */

/* SDL reaches the APK's assets through the AssetManager, in two forms: an
 * AssetFileDescriptor when the entry is stored uncompressed, and an
 * InputStream wrapped in a ReadableByteChannel otherwise.  Both end up reading
 * a real file from the extracted asset tree here, so both are answered with a
 * real file descriptor rather than an emulated stream. */
static int open_asset_fd(jctx *c, char *path, size_t path_size, off_t *size_out)
{
    jobj *name = jarg_obj(c);
    const char *rel = name && name->str ? name->str : "";
    while (*rel == '/')
        rel++;
    if (strncmp(rel, "assets/", 7) == 0)
        rel += 7;
    snprintf(path, path_size, "%s/%s", tr_datadir, rel);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        JT("asset \"%s\" -> not found", rel);
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return -1;
    }
    *size_out = st.st_size;
    JT("asset \"%s\" -> %s (%lld bytes)", rel, path, (long long)st.st_size);
    return fd;
}

static jobj *mk_fd_object(const char *cls, int fd, off_t size, const char *path)
{
    jobj *o = mk_object(cls);
    o->prim = fd;
    o->len = (int)size;
    o->str = strdup(path);
    return o;
}

static int64_t j_AssetManager_openFd(jctx *c)
{
    char path[1280];
    off_t size = 0;
    int fd = open_asset_fd(c, path, sizeof path, &size);
    if (fd < 0)
        return 0;
    return (int64_t)(uintptr_t)
        mk_fd_object("android/content/res/AssetFileDescriptor", fd, size, path);
}

static int64_t j_AssetManager_open(jctx *c)
{
    char path[1280];
    off_t size = 0;
    int fd = open_asset_fd(c, path, sizeof path, &size);
    if (fd < 0)
        return 0;
    return (int64_t)(uintptr_t)
        mk_fd_object("java/io/InputStream", fd, size, path);
}

static int64_t j_afd_startOffset(jctx *c)
{
    (void)c;
    /* The tree is extracted, so every asset starts at offset zero of its own
     * file rather than at an offset inside the APK. */
    return 0;
}

static int64_t j_afd_length(jctx *c)
{
    return c->self ? c->self->len : 0;
}

static int64_t j_afd_descriptor(jctx *c)
{
    if (!c->self)
        return 0;
    jobj *o = mk_object("android/os/ParcelFileDescriptor");
    o->prim = c->self->prim;
    return (int64_t)(uintptr_t)o;
}

static int64_t j_fd_getFd(jctx *c)
{
    return c->self ? c->self->prim : -1;
}

static int64_t j_fd_detachFd(jctx *c)
{
    if (!c->self)
        return -1;
    /* detachFd() hands ownership over, so give the caller its own copy and
     * keep ours closable. */
    return dup((int)c->self->prim);
}

static int64_t j_stream_available(jctx *c)
{
    if (!c->self)
        return 0;
    off_t here = lseek((int)c->self->prim, 0, SEEK_CUR);
    return here < 0 ? 0 : (int64_t)(c->self->len - here);
}

static int64_t j_stream_skip(jctx *c)
{
    int64_t n = jarg_long(c);
    if (!c->self)
        return 0;
    off_t before = lseek((int)c->self->prim, 0, SEEK_CUR);
    off_t after = lseek((int)c->self->prim, n, SEEK_CUR);
    return after < 0 ? 0 : (int64_t)(after - before);
}

static int64_t j_stream_close(jctx *c)
{
    if (c->self && c->self->prim >= 0) {
        close((int)c->self->prim);
        c->self->prim = -1;
    }
    return 0;
}

static int64_t j_stream_read(jctx *c)
{
    jobj *arr = jarg_obj(c);
    int32_t off = 0, len = arr ? arr->len : 0;
    if (c->m && strcmp(c->m->sig, "([BII)I") == 0) {
        off = jarg_int(c);
        len = jarg_int(c);
    }
    if (!c->self || !arr || !arr->data)
        return -1;
    ssize_t got = read((int)c->self->prim, (char *)arr->data + off, (size_t)len);
    return got > 0 ? got : -1;
}

/* Channels.newChannel(stream) just returns the stream itself: the object
 * already carries the descriptor, and read(ByteBuffer) below is the only
 * method SDL calls on the channel. */
static int64_t j_Channels_newChannel(jctx *c)
{
    return (int64_t)(uintptr_t)jarg_obj(c);
}

static int64_t j_channel_read(jctx *c)
{
    jobj *buf = jarg_obj(c);
    if (!c->self || !buf || !buf->data)
        return -1;
    ssize_t got = read((int)c->self->prim, buf->data, (size_t)buf->len);
    return got > 0 ? got : -1;
}

static int64_t j_Scanner_next(jctx *c)
{
    const char *path = c->self ? c->self->str : NULL;
    if (!path)
        return (int64_t)(uintptr_t)mk_string("");
    FILE *f = fopen(path, "rb");
    if (!f)
        return (int64_t)(uintptr_t)mk_string("");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    if (n < 0 || n > 1024 * 1024) {
        fclose(f);
        return (int64_t)(uintptr_t)mk_string("");
    }
    char *buf = calloc((size_t)n + 1, 1);
    if (buf && n)
        (void)fread(buf, 1, (size_t)n, f);
    fclose(f);
    jobj *s = mk_string(buf ? buf : "");
    free(buf);
    return (int64_t)(uintptr_t)s;
}

static int64_t j_Environment_mounted(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)mk_string("mounted");
}

static int64_t j_String_equals(jctx *c)
{
    jobj *other = jarg_obj(c);
    const char *a = c->self && c->self->str ? c->self->str : "";
    const char *b = other && other->str ? other->str : "";
    return strcmp(a, b) == 0;
}

static int64_t j_StringBuilder_toString(jctx *c)
{
    return (int64_t)(uintptr_t)mk_string(
        c->self && c->self->str ? c->self->str : "");
}

static int uri_unreserved(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           c == '.' || c == '~';
}

static int64_t j_Uri_encode(jctx *c)
{
    jobj *input = jarg_obj(c);
    const char *text = input && input->str ? input->str : "";
    size_t length = strlen(text);
    char *encoded = malloc(length * 3 + 1);
    static const char hex[] = "0123456789ABCDEF";
    size_t out = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char)text[i];
        if (uri_unreserved(byte)) {
            encoded[out++] = (char)byte;
        } else {
            encoded[out++] = '%';
            encoded[out++] = hex[byte >> 4];
            encoded[out++] = hex[byte & 15];
        }
    }
    encoded[out] = '\0';
    jobj *result = mk_string(encoded);
    free(encoded);
    return (int64_t)(uintptr_t)result;
}

static int uri_hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int64_t j_Uri_decode(jctx *c)
{
    jobj *input = jarg_obj(c);
    const char *text = input && input->str ? input->str : "";
    size_t length = strlen(text);
    char *decoded = malloc(length + 1);
    size_t out = 0;
    for (size_t i = 0; i < length; i++) {
        int high, low;
        if (text[i] == '%' && i + 2 < length &&
            (high = uri_hex(text[i + 1])) >= 0 &&
            (low = uri_hex(text[i + 2])) >= 0) {
            decoded[out++] = (char)((high << 4) | low);
            i += 2;
        } else {
            decoded[out++] = text[i];
        }
    }
    decoded[out] = '\0';
    jobj *result = mk_string(decoded);
    free(decoded);
    return (int64_t)(uintptr_t)result;
}

/* SDL reads android.os.Build to pick device-specific workarounds, and the
 * first thing it does with MANUFACTURER is strcasecmp(field, "Amazon") -- with
 * the field absent that faults inside strcasecmp before anything is drawn.
 *
 * These values describe this device, and deliberately match no vendor SDL
 * has a workaround for: a name borrowed from a real manufacturer would switch
 * on a code path written for someone else's hardware.  The field name arrives
 * in the context, so one handler serves the whole class.
 */
static const struct { const char *name, *value; } build_fields[] = {
    { "MANUFACTURER", "NextOS" },
    { "BRAND",        "NextOS" },
    { "MODEL",        "NextOS-Retro-Elite" },
    { "PRODUCT",      "nextos" },
    { "DEVICE",       "amlogic" },
    { "BOARD",        "amlogic" },
    { "HARDWARE",     "amlogic" },
    { "ID",           "NextOS" },
    { "TYPE",         "user" },
    { "TAGS",         "release-keys" },
    { "FINGERPRINT",  "NextOS/nextos/amlogic:13/NextOS/4.8.2:user/release-keys" },
    { "RELEASE",      "13" },
    { "CODENAME",     "REL" },
    { "INCREMENTAL",  "4.8.2" },
    { "SERIAL",       "unknown" },
};

static int64_t j_Build_string(jctx *c)
{
    const char *name = c->m ? c->m->name : NULL;
    if (name)
        for (size_t i = 0; i < sizeof build_fields / sizeof *build_fields; i++)
            if (strcmp(build_fields[i].name, name) == 0)
                return (int64_t)(uintptr_t)mk_string(build_fields[i].value);
    /* Never NULL: the caller passes these straight to string functions. */
    return (int64_t)(uintptr_t)mk_string("");
}

/* API 33 is the platform this 2022.3 player was built against.  Zero would read
 * as older than every gate SDL checks and take paths this build never
 * compiled. */
static int64_t j_Build_SDK_INT(jctx *c)
{
    (void)c;
    return 33;
}

/* A typed, persistent SharedPreferences store.
 *
 * Lime keeps its own save data in the app's files directory, but the Android
 * surface still answers preference reads, and anything the game writes has to
 * survive a restart -- the port has no Java runtime to fall back on.
 */
enum pref_type {
    PREF_INT = 1,
    PREF_FLOAT,
    PREF_STRING,
    PREF_BOOL,
};

typedef struct {
    char *key;
    enum pref_type type;
    int32_t integer;
    float real;
    char *string;
} pref_entry;

#define MAX_PREFS 1024
static pref_entry preferences[MAX_PREFS];
static int preferences_count;
static int preferences_loaded;
static pthread_mutex_t preferences_lock = PTHREAD_MUTEX_INITIALIZER;

static pref_entry *pref_find_locked(const char *key)
{
    for (int i = 0; i < preferences_count; i++)
        if (strcmp(preferences[i].key, key) == 0)
            return &preferences[i];
    return NULL;
}

static pref_entry *pref_set_locked(const char *key, enum pref_type type)
{
    pref_entry *entry = pref_find_locked(key);
    if (!entry) {
        if (preferences_count >= MAX_PREFS)
            return NULL;
        entry = &preferences[preferences_count++];
        memset(entry, 0, sizeof *entry);
        entry->key = strdup(key);
    }
    if (entry->type == PREF_STRING) {
        free(entry->string);
        entry->string = NULL;
    }
    entry->type = type;
    return entry;
}

static int pref_write(FILE *file, const void *data, size_t bytes)
{
    return fwrite(data, 1, bytes, file) == bytes;
}

static int pref_read(FILE *file, void *data, size_t bytes)
{
    return fread(data, 1, bytes, file) == bytes;
}

static int preferences_save_locked(void)
{
    char path[1200];
    char temporary[1200];
    snprintf(path, sizeof path, "%s/shared-preferences.bin", tr_home);
    snprintf(temporary, sizeof temporary,
             "%s/shared-preferences.bin.tmp", tr_home);
    FILE *file = fopen(temporary, "wb");
    if (!file)
        return 0;

    static const unsigned char magic[8] =
        { 'P', 'F', '2', 'P', 'R', 'E', 'F', '1' };
    uint32_t count = (uint32_t)preferences_count;
    int ok = pref_write(file, magic, sizeof magic) &&
             pref_write(file, &count, sizeof count);
    for (int i = 0; ok && i < preferences_count; i++) {
        pref_entry *entry = &preferences[i];
        uint8_t type = (uint8_t)entry->type;
        uint32_t key_length = (uint32_t)strlen(entry->key);
        uint32_t value_length =
            entry->type == PREF_STRING
                ? (uint32_t)strlen(entry->string ? entry->string : "")
                : (uint32_t)sizeof(uint32_t);
        ok = pref_write(file, &type, sizeof type) &&
             pref_write(file, &key_length, sizeof key_length) &&
             pref_write(file, &value_length, sizeof value_length) &&
             pref_write(file, entry->key, key_length);
        if (!ok)
            break;
        if (entry->type == PREF_STRING)
            ok = pref_write(file, entry->string ? entry->string : "",
                            value_length);
        else if (entry->type == PREF_FLOAT)
            ok = pref_write(file, &entry->real, sizeof entry->real);
        else
            ok = pref_write(file, &entry->integer, sizeof entry->integer);
    }
    if (fflush(file) != 0 || fsync(fileno(file)) != 0)
        ok = 0;
    if (fclose(file) != 0)
        ok = 0;
    if (ok && rename(temporary, path) != 0)
        ok = 0;
    if (!ok)
        unlink(temporary);
    return ok;
}

static void preferences_load_locked(void)
{
    if (preferences_loaded)
        return;
    preferences_loaded = 1;

    char path[1200];
    snprintf(path, sizeof path, "%s/shared-preferences.bin", tr_home);
    FILE *file = fopen(path, "rb");
    if (!file)
        return;

    unsigned char magic[8];
    static const unsigned char expected[8] =
        { 'P', 'F', '2', 'P', 'R', 'E', 'F', '1' };
    uint32_t count;
    if (!pref_read(file, magic, sizeof magic) ||
        memcmp(magic, expected, sizeof magic) != 0 ||
        !pref_read(file, &count, sizeof count) ||
        count > MAX_PREFS) {
        fclose(file);
        nx_log("prefs: ignored invalid %s", path);
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint8_t type;
        uint32_t key_length;
        uint32_t value_length;
        if (!pref_read(file, &type, sizeof type) ||
            !pref_read(file, &key_length, sizeof key_length) ||
            !pref_read(file, &value_length, sizeof value_length) ||
            key_length == 0 || key_length > 4096 ||
            value_length > 1024 * 1024 ||
            type < PREF_INT || type > PREF_BOOL)
            break;
        char *key = calloc((size_t)key_length + 1, 1);
        char *value = calloc((size_t)value_length + 1, 1);
        if (!key || !value ||
            !pref_read(file, key, key_length) ||
            !pref_read(file, value, value_length)) {
            free(key);
            free(value);
            break;
        }
        pref_entry *entry = pref_set_locked(key, (enum pref_type)type);
        if (entry) {
            if (type == PREF_STRING) {
                entry->string = value;
                value = NULL;
            } else if (value_length == sizeof(uint32_t)) {
                if (type == PREF_FLOAT)
                    memcpy(&entry->real, value, sizeof entry->real);
                else
                    memcpy(&entry->integer, value, sizeof entry->integer);
            }
        }
        free(key);
        free(value);
    }
    fclose(file);
    nx_log("prefs: loaded %d persistent entries", preferences_count);
}

static int64_t j_getPreferences(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)preferences_object;
}

static int64_t j_Prefs_getBoolean(jctx *c)
{
    jobj *key = jarg_obj(c);
    int32_t dflt = jarg_int(c);
    const char *k = key && key->str ? key->str : "";
    if (strcmp(k, "gles-api-check") == 0)
        return 1;
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    pref_entry *entry = pref_find_locked(k);
    int value = entry && entry->type == PREF_BOOL
        ? entry->integer != 0 : dflt != 0;
    pthread_mutex_unlock(&preferences_lock);
    JT("SharedPreferences.getBoolean(\"%s\", %d) -> %d", k, dflt, value);
    return value;
}

static int64_t j_Prefs_getInt(jctx *c)
{
    jobj *key = jarg_obj(c);
    int32_t dflt = jarg_int(c);
    const char *k = key && key->str ? key->str : "";
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    pref_entry *entry = pref_find_locked(k);
    int32_t value = entry && entry->type == PREF_INT
        ? entry->integer : dflt;
    pthread_mutex_unlock(&preferences_lock);
    JT("SharedPreferences.getInt(\"%s\", %d) -> %d", k, dflt, value);
    return value;
}

static int64_t j_Prefs_getFloat(jctx *c)
{
    jobj *key = jarg_obj(c);
    float dflt = jarg_float(c);
    const char *k = key && key->str ? key->str : "";
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    pref_entry *entry = pref_find_locked(k);
    float value = entry && entry->type == PREF_FLOAT
        ? entry->real : dflt;
    pthread_mutex_unlock(&preferences_lock);
    JT("SharedPreferences.getFloat(\"%s\", %.3f) -> %.3f", k, dflt, value);
    return jfloat_result(value);
}

static int64_t j_Prefs_getString(jctx *c)
{
    jobj *key = jarg_obj(c);
    jobj *dflt = jarg_obj(c);
    const char *k = key && key->str ? key->str : "";
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    pref_entry *entry = pref_find_locked(k);
    jobj *value = entry && entry->type == PREF_STRING
        ? mk_string(entry->string ? entry->string : "") : dflt;
    pthread_mutex_unlock(&preferences_lock);
    return (int64_t)(uintptr_t)value;
}

static int64_t j_Prefs_contains(jctx *c)
{
    jobj *key = jarg_obj(c);
    const char *k = key && key->str ? key->str : "";
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    int value = pref_find_locked(k) != NULL;
    pthread_mutex_unlock(&preferences_lock);
    JT("SharedPreferences.contains(\"%s\") -> %d", k, value);
    return value;
}

static int64_t j_Prefs_edit(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)preferences_editor;
}

static int64_t j_Prefs_putInt(jctx *c)
{
    jobj *key = jarg_obj(c);
    int32_t value = jarg_int(c);
    const char *k = key && key->str ? key->str : "";
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    pref_entry *entry = pref_set_locked(k, PREF_INT);
    if (entry)
        entry->integer = value;
    pthread_mutex_unlock(&preferences_lock);
    JT("SharedPreferences.putInt(\"%s\", %d)", k, value);
    return (int64_t)(uintptr_t)preferences_editor;
}

static int64_t j_Prefs_putFloat(jctx *c)
{
    jobj *key = jarg_obj(c);
    float value = jarg_float(c);
    const char *k = key && key->str ? key->str : "";
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    pref_entry *entry = pref_set_locked(k, PREF_FLOAT);
    if (entry)
        entry->real = value;
    pthread_mutex_unlock(&preferences_lock);
    JT("SharedPreferences.putFloat(\"%s\", %.3f)", k, value);
    return (int64_t)(uintptr_t)preferences_editor;
}

static int64_t j_Prefs_putString(jctx *c)
{
    jobj *key = jarg_obj(c);
    jobj *value = jarg_obj(c);
    const char *k = key && key->str ? key->str : "";
    const char *v = value && value->str ? value->str : "";
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    pref_entry *entry = pref_set_locked(k, PREF_STRING);
    if (entry)
        entry->string = strdup(v);
    pthread_mutex_unlock(&preferences_lock);
    JT("SharedPreferences.putString(\"%s\", %d bytes)", k, (int)strlen(v));
    return (int64_t)(uintptr_t)preferences_editor;
}

static int64_t j_Prefs_putBoolean(jctx *c)
{
    jobj *key = jarg_obj(c);
    int32_t value = jarg_int(c);
    const char *k = key && key->str ? key->str : "";
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    pref_entry *entry = pref_set_locked(k, PREF_BOOL);
    if (entry)
        entry->integer = value != 0;
    pthread_mutex_unlock(&preferences_lock);
    return (int64_t)(uintptr_t)preferences_editor;
}

static int64_t j_Prefs_remove(jctx *c)
{
    jobj *key = jarg_obj(c);
    const char *k = key && key->str ? key->str : "";
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    for (int i = 0; i < preferences_count; i++) {
        if (strcmp(preferences[i].key, k) != 0)
            continue;
        free(preferences[i].key);
        free(preferences[i].string);
        memmove(&preferences[i], &preferences[i + 1],
                (size_t)(preferences_count - i - 1) *
                    sizeof preferences[0]);
        preferences_count--;
        break;
    }
    pthread_mutex_unlock(&preferences_lock);
    return (int64_t)(uintptr_t)preferences_editor;
}

static int64_t j_Prefs_clear(jctx *c)
{
    (void)c;
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    for (int i = 0; i < preferences_count; i++) {
        free(preferences[i].key);
        free(preferences[i].string);
    }
    preferences_count = 0;
    pthread_mutex_unlock(&preferences_lock);
    return (int64_t)(uintptr_t)preferences_editor;
}

static int64_t j_Prefs_apply(jctx *c)
{
    (void)c;
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    int ok = preferences_save_locked();
    pthread_mutex_unlock(&preferences_lock);
    if (!ok)
        nx_log("prefs: cannot persist settings under %s", tr_home);
    return 0;
}

static int64_t j_Prefs_commit(jctx *c)
{
    j_Prefs_apply(c);
    return 1;
}

static int64_t j_Object_getClass(jctx *c)
{
    return (int64_t)(uintptr_t)mk_class(
        c->self && c->self->cls ? c->self->cls : "java/lang/Object");
}

static int64_t j_getClassLoader(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)mk_object("java/lang/ClassLoader");
}

static int64_t j_ClassLoader_findLibrary(jctx *c)
{
    jobj *name = jarg_obj(c);
    const char *lib = name && name->str ? name->str : "";
    const char *file = NULL;

    if (strcmp(lib, "lime") == 0 || strcmp(lib, "liblime.so") == 0)
        file = "liblime.so";
    else if (strcmp(lib, "ApplicationMain") == 0 ||
             strcmp(lib, "libApplicationMain.so") == 0)
        file = "libApplicationMain.so";

    if (!file) {
        JT("ClassLoader.findLibrary(\"%s\") -> empty", lib);
        return (int64_t)(uintptr_t)mk_string("");
    }

    char path[1280];
    snprintf(path, sizeof path, "%s/lib/%s", tr_gamedir, file);
    JT("ClassLoader.findLibrary(\"%s\") -> %s", lib, path);
    return (int64_t)(uintptr_t)mk_string(path);
}

static int64_t j_System_loadLibrary(jctx *c)
{
    jobj *name = jarg_obj(c);
    JT("System.%s(\"%s\") -> already loaded",
       c->m && c->m->name ? c->m->name : "loadLibrary",
       name && name->str ? name->str : "");
    return 0;
}

static int64_t j_forName(jctx *c)
{
    jobj *name = jarg_obj(c);
    (void)jarg_int(c);
    (void)jarg_obj(c);
    const char *n = name && name->str ? name->str : "java/lang/Object";
    char cls[256];
    snprintf(cls, sizeof cls, "%s", n);
    for (char *p = cls; *p; p++)
        if (*p == '.')
            *p = '/';
    return (int64_t)(uintptr_t)mk_class(cls);
}










static int64_t j_System_nanoTime(jctx *c)
{
    (void)c;
    return monotonic_nanos();
}



static int64_t j_Method_getName(jctx *c)
{
    return (int64_t)(uintptr_t)mk_string(
        c->self && c->self->str ? c->self->str : "");
}






/* An object-returning call that answered NULL is the shape of bug that shows
 * up much later as "null object reference" inside code we cannot step through,
 * so name it where it happens. */
/* --------------------------------------------------------------- sem anuncio
 *
 * O jogo fala com UnityAds, AdMob/consentimento, Play Games e Billing.  O
 * default do dispatch ja devolve 0 para tudo isso ("nada disponivel"), o que
 * basta para o jogo seguir -- mas so por acidente: 0 tambem e o valor de
 * "ainda nao sei", e uma tela que espera resposta definitiva fica esperando.
 * Aqui a resposta vira EXPLICITA: nao ha anuncio, nao havera, siga.
 *
 * Medido antes de escrever: isSignedIn e chamado 1x por frame (polling barato,
 * nao e a causa do soluco de 2,4s); init de UnityAds/Billing, 1x cada. */
static int64_t j_ads_false(jctx *c) { (void)c; return 0; }
static int64_t j_ads_true(jctx *c)  { (void)c; return 1; }

void tr_jni_bind_ads(void)
{
    /* "carregou?" / "esta pronto?" -> NAO, definitivamente */
    tr_jni_bind("com/byrobin/unityads/UnityAdsEx", "isReady",
                "(Ljava/lang/String;)Z", (void *)j_ads_false);
    tr_jni_bind("com/byrobin/unityads/UnityAdsEx", "isSupported", "()Z",
                (void *)j_ads_false);
    tr_jni_bind("com/byrobin/unityads/UnityAdsEx", "isInitialized", "()Z",
                (void *)j_ads_true);   /* init TERMINOU (sem anuncio) */

    /* Play Games: nunca logado, e a pergunta ja tem resposta final. */
    tr_jni_bind("com/stencyl/GoogleServices/GooglePlayGames", "isSignedIn",
                "()Z", (void *)j_ads_false);

    /* Billing: sem compras, sem "Extra Theatres" pago.  O jogo trata a
     * ausencia como conteudo bloqueado, que e o comportamento honesto de um
     * aparelho sem loja. */
    tr_jni_bind("com/stencyl/android/AndroidBilling", "isAvailable", "()Z",
                (void *)j_ads_false);
}

static int returns_object(const jmethod *m)
{
    const char *ret = m && m->sig ? strchr(m->sig, ')') : NULL;
    if (!ret)
        ret = m && m->sig ? m->sig : NULL;   /* a field descriptor */
    else
        ret++;
    return ret && (*ret == 'L' || *ret == '[');
}

static int64_t dispatch(void *e, jobj *self, void *mid, va_list *ap,
                        const uint64_t *args)
{
    const jmethod *m = by_id(mid);
    if (!m) {
        JT("call on unknown method id %p", mid);
        return 0;
    }
    if (self && self->boxed && is_unbox(m->name)) {
        JT("unboxed %s.%s -> %lld", self->cls, m->name, (long long)self->prim);
        return self->prim;
    }
    if (m->handler) {
        jctx c = {
            .env = e,
            .self = self,
            .ap = ap,
            .args = args,
            .arg_index = 0,
            .m = m,
        };
        int64_t r = ((tr_jni_handler)m->handler)(&c);
        if (!r && returns_object(m))
            JT("NULL from %s.%s%s", m->cls ? m->cls : "?", m->name, m->sig);
        return r;
    }
    if (returns_object(m))
        JT("NULL (no handler) from %s.%s%s", m->cls ? m->cls : "?", m->name,
           m->sig);
    /* A method we do not implement whose return type is the receiver's own
     * class is a builder -- Intent.putExtra, setPackage, setFlags, setData all
     * return the Intent so calls can be chained.  Returning NULL breaks the
     * chain mid-expression and every later link then operates on nothing;
     * returning the receiver is what the platform does. */
    if (self && self->cls) {
        const char *ret = strchr(m->sig, ')');
        size_t n = strlen(self->cls);
        if (ret && ret[1] == 'L' && strncmp(ret + 2, self->cls, n) == 0 &&
            ret[2 + n] == ';') {
            JT("builder %s.%s -> self", self->cls, m->name);
            return (int64_t)(uintptr_t)self;
        }
    }
    JT("unhandled %s.%s%s -> 0", m->cls ? m->cls : "?", m->name, m->sig);
    return 0;
}

/* Argument readers for handlers. */
void *tr_jarg_obj(jctx *c) { return jarg_obj(c); }
int32_t tr_jarg_int(jctx *c) { return jarg_int(c); }
int64_t tr_jarg_long(jctx *c) { return jarg_long(c); }
const char *tr_jarg_str(jctx *c)
{
    jobj *o = jarg_obj(c);
    return o && o->str ? o->str : NULL;
}
void *tr_jret_str(const char *s) { return mk_string(s); }
void *tr_jret_class(const char *s) { return mk_class(s); }
void *tr_jret_obj(const char *cls) { return new_obj(O_OBJECT, cls); }
int64_t tr_jret_float(float v)
{
    int64_t r = 0;
    memcpy(&r, &v, sizeof v);
    return r;
}
const char *tr_jmethod_name(jctx *c)
{
    return c && c->m && c->m->name ? c->m->name : "";
}
void *tr_jret_bytes(const void *p, int n)
{
    jobj *o = j_NewByteArray(NULL, n);
    if (p && n > 0)
        memcpy(o->data, p, (size_t)n);
    return o;
}

#define CALLV(nm, rt, cast) \
    static rt nm(void *e, void *o, void *mid, va_list ap) \
    { return (rt)(cast)dispatch(e, o, mid, &ap, NULL); }
#define CALL(nm, vnm, rt) \
    static rt nm(void *e, void *o, void *mid, ...) \
    { va_list ap; va_start(ap, mid); rt r = vnm(e, o, mid, ap); va_end(ap); return r; }
#define CALLA(nm, rt) \
    static rt nm(void *e, void *o, void *mid, void *args) \
    { return (rt)dispatch(e, o, mid, NULL, (const uint64_t *)args); }

CALLV(v_obj,  void *,   uintptr_t)
CALLV(v_bool, uint8_t,  uint8_t)
CALLV(v_byte, int8_t,   int8_t)
CALLV(v_char, uint16_t, uint16_t)
CALLV(v_short,int16_t,  int16_t)
CALLV(v_int,  int32_t,  int32_t)
CALLV(v_long, int64_t,  int64_t)

static float v_float(void *e, void *o, void *mid, va_list ap)
{
    int64_t r = dispatch(e, o, mid, &ap, NULL);
    float f;
    memcpy(&f, &r, sizeof f);
    return f;
}
static double v_double(void *e, void *o, void *mid, va_list ap)
{
    int64_t r = dispatch(e, o, mid, &ap, NULL);
    double d;
    memcpy(&d, &r, sizeof d);
    return d;
}
static void v_void(void *e, void *o, void *mid, va_list ap)
{
    dispatch(e, o, mid, &ap, NULL);
}

CALL(c_obj, v_obj, void *)
CALL(c_bool, v_bool, uint8_t)
CALL(c_byte, v_byte, int8_t)
CALL(c_char, v_char, uint16_t)
CALL(c_short, v_short, int16_t)
CALL(c_int, v_int, int32_t)
CALL(c_long, v_long, int64_t)
CALL(c_float, v_float, float)
CALL(c_double, v_double, double)
static void c_void(void *e, void *o, void *mid, ...)
{
    va_list ap;
    va_start(ap, mid);
    v_void(e, o, mid, ap);
    va_end(ap);
}

CALLA(a_obj, void *)
CALLA(a_bool, uint8_t)
CALLA(a_byte, int8_t)
CALLA(a_char, uint16_t)
CALLA(a_short, int16_t)
CALLA(a_int, int32_t)
CALLA(a_long, int64_t)
static float a_float(void *e, void *o, void *m, void *v)
{
    int64_t r = dispatch(e, o, m, NULL, (const uint64_t *)v);
    float f;
    memcpy(&f, &r, sizeof f);
    return f;
}
static double a_double(void *e, void *o, void *m, void *v)
{
    int64_t r = dispatch(e, o, m, NULL, (const uint64_t *)v);
    double d;
    memcpy(&d, &r, sizeof d);
    return d;
}
static void a_void(void *e, void *o, void *m, void *v)
{
    (void)dispatch(e, o, m, NULL, (const uint64_t *)v);
}

static void *j_GetMethodID(void *e, jobj *c, const char *n, const char *s)
{
    (void)e;
    return method_id(c ? c->cls : NULL, n, s);
}
static void *j_GetFieldID(void *e, jobj *c, const char *n, const char *s)
{
    (void)e;
    return method_id(c ? c->cls : NULL, n, s);
}

static jobj *j_AllocObject(void *e, jobj *c)
{
    (void)e;
    return new_obj(O_OBJECT, c ? c->cls : NULL);
}

/* The boxed primitives have to carry their value.  PairIP's ExecuteProgram boxes
 * the raw arguments it was given -- for a protected DT_INIT that is the module
 * base address as a java/lang/Long -- and the VM reads them back with
 * longValue().  A constructor that drops its argument hands the VM a zero base,
 * which it then uses as an offset into the program and walks off the end. */
static int box_kind(const char *cls)
{
    if (!cls)
        return 0;
    if (strcmp(cls, "java/lang/Long") == 0)      return 'J';
    if (strcmp(cls, "java/lang/Integer") == 0)   return 'I';
    if (strcmp(cls, "java/lang/Short") == 0)     return 'S';
    if (strcmp(cls, "java/lang/Byte") == 0)      return 'B';
    if (strcmp(cls, "java/lang/Character") == 0) return 'C';
    if (strcmp(cls, "java/lang/Boolean") == 0)   return 'Z';
    if (strcmp(cls, "java/lang/Float") == 0)     return 'F';
    if (strcmp(cls, "java/lang/Double") == 0)    return 'D';
    return 0;
}

static jobj *box_from(jobj *c, void *mid, va_list *ap)
{
    jobj *o = new_obj(O_OBJECT, c ? c->cls : NULL);
    int k = box_kind(o->cls);
    if (!k) {
        /* Not a boxed primitive.  Keep a single String argument: java/io/File
         * and android/content/Intent are both built from one and then asked
         * about it, and an object that forgot its own path is indistinguishable
         * from a file that does not exist. */
        const jmethod *m = by_id(mid);
        if (m && strcmp(m->sig, "(Ljava/lang/String;)V") == 0) {
            jobj *s = va_arg(*ap, jobj *);
            if (s && s->str) {
                o->str = strdup(s->str);
                o->len = (int)strlen(o->str);
            }
        } else if (m && o->cls &&
                   strcmp(o->cls, "java/lang/String") == 0 &&
                   (strcmp(m->sig, "([BLjava/lang/String;)V") == 0 ||
                    strcmp(m->sig, "([B)V") == 0)) {
            /* Haxe marshals its UTF-8 strings by constructing a Java
             * String from byte[] plus the "UTF-8" charset name.  Dropping
             * that byte array turns every PlayerPrefs key into "", causing
             * all settings -- including the four audio volumes -- to alias. */
            jobj *bytes = va_arg(*ap, jobj *);
            if (strcmp(m->sig, "([BLjava/lang/String;)V") == 0)
                (void)va_arg(*ap, jobj *);
            if (bytes && bytes->data && bytes->len >= 0) {
                o->str = calloc((size_t)bytes->len + 1, 1);
                memcpy(o->str, bytes->data, (size_t)bytes->len);
                o->len = bytes->len;
            }
        } else if (m && o->cls &&
                   strcmp(o->cls, "java/util/Scanner") == 0 &&
                   strcmp(m->sig,
                          "(Ljava/io/InputStream;Ljava/lang/String;)V") == 0) {
            jobj *stream = va_arg(*ap, jobj *);
            (void)va_arg(*ap, jobj *);
            if (stream && stream->str) {
                o->str = strdup(stream->str);
                o->len = (int)strlen(o->str);
            }
        }
        JT("new %s(\"%s\")", o->cls ? o->cls : "?", o->str ? o->str : "");
        return o;
    }
    o->boxed = k;
    switch (k) {
    case 'J': o->prim = va_arg(*ap, int64_t); break;
    case 'I': o->prim = va_arg(*ap, int32_t); break;
    /* short, byte, char and boolean are promoted to int in a varargs call. */
    case 'S': o->prim = (int16_t)va_arg(*ap, int32_t); break;
    case 'B': o->prim = (int8_t)va_arg(*ap, int32_t); break;
    case 'C': o->prim = (uint16_t)va_arg(*ap, int32_t); break;
    case 'Z': o->prim = va_arg(*ap, int32_t) ? 1 : 0; break;
    case 'F': { double f = va_arg(*ap, double); float g = (float)f;
                memcpy(&o->prim, &g, sizeof g); break; }
    case 'D': { double f = va_arg(*ap, double);
                memcpy(&o->prim, &f, sizeof f); break; }
    default: break;
    }
    JT("boxed %s = %lld", o->cls, (long long)o->prim);
    return o;
}

static jobj *v_NewObject(void *e, jobj *c, void *mid, va_list ap)
{
    (void)e;
    return box_from(c, mid, &ap);
}
static jobj *j_NewObject(void *e, jobj *c, void *mid, ...)
{
    (void)e;
    va_list ap;
    va_start(ap, mid);
    jobj *o = box_from(c, mid, &ap);
    va_end(ap);
    return o;
}
static jobj *a_NewObject(void *e, jobj *c, void *mid, void *v)
{
    /* The A form passes a jvalue array, one slot per argument.  The VM builds
     * its java/io/File through this form and its Intent through the varargs
     * one, so the String argument has to be kept here too -- covering only the
     * varargs path leaves the File with no path at all. */
    (void)e;
    jobj *o = new_obj(O_OBJECT, c ? c->cls : NULL);
    int k = box_kind(o->cls);
    if (!k) {
        const jmethod *m = by_id(mid);
        if (m && v && strcmp(m->sig, "(Ljava/lang/String;)V") == 0) {
            jobj *s = *(jobj **)v;
            if (s && s->str) {
                o->str = strdup(s->str);
                o->len = (int)strlen(o->str);
            }
        } else if (m && v && o->cls &&
                   strcmp(o->cls, "java/lang/String") == 0 &&
                   (strcmp(m->sig, "([BLjava/lang/String;)V") == 0 ||
                    strcmp(m->sig, "([B)V") == 0)) {
            jobj *bytes = *(jobj **)v;
            if (bytes && bytes->data && bytes->len >= 0) {
                o->str = calloc((size_t)bytes->len + 1, 1);
                memcpy(o->str, bytes->data, (size_t)bytes->len);
                o->len = bytes->len;
            }
        }
        JT("new %s(\"%s\") [A]", o->cls ? o->cls : "?", o->str ? o->str : "");
        return o;
    }
    if (k && v) {
        o->boxed = k;
        o->prim = *(const int64_t *)v;
        if (k == 'I') o->prim = (int32_t)o->prim;
        if (k == 'S') o->prim = (int16_t)o->prim;
        if (k == 'B') o->prim = (int8_t)o->prim;
        if (k == 'C') o->prim = (uint16_t)o->prim;
        if (k == 'Z') o->prim = o->prim ? 1 : 0;
    }
    return o;
}

/* Fields all go through the same dispatch, so a getter can be bound the same
 * way a method is; anything unbound reads as zero/NULL. */
static void *f_obj(void *e, jobj *o, void *fid)
{
    return (void *)(uintptr_t)dispatch(e, o, fid, NULL, NULL);
}
static uint8_t f_bool(void *e, jobj *o, void *f) { return (uint8_t)(uintptr_t)f_obj(e, o, f); }
static int8_t f_byte(void *e, jobj *o, void *f) { return (int8_t)(uintptr_t)f_obj(e, o, f); }
static uint16_t f_char(void *e, jobj *o, void *f) { return (uint16_t)(uintptr_t)f_obj(e, o, f); }
static int16_t f_short(void *e, jobj *o, void *f) { return (int16_t)(uintptr_t)f_obj(e, o, f); }
static int32_t f_int(void *e, jobj *o, void *f) { return (int32_t)(uintptr_t)f_obj(e, o, f); }
static int64_t f_long(void *e, jobj *o, void *f) { return (int64_t)(uintptr_t)f_obj(e, o, f); }
/* A float field handler returns the bit pattern in the int64 slot, exactly as
 * the float call handlers do; reading it as an integer would turn a DPI of
 * 160.0 into 1127219200. */
static float f_float(void *e, jobj *o, void *fid)
{
    int64_t r = dispatch(e, o, fid, NULL, NULL);
    float v;
    memcpy(&v, &r, sizeof v);
    return v;
}
static double f_double(void *e, jobj *o, void *fid)
{
    int64_t r = dispatch(e, o, fid, NULL, NULL);
    double v;
    memcpy(&v, &r, sizeof v);
    return v;
}
static void f_set(void *e, jobj *o, void *f, ...) { (void)e; (void)o; (void)f; }

static int32_t j_RegisterNatives(void *e, jobj *c, const void *m, int32_t n)
{
    (void)e;
    const struct { const char *name; const char *sig; void *fn; } *r = m;
    for (int32_t i = 0; i < n; i++) {
        if (natives_n == MAX_NATIVES)
            nx_die("native table full");
        jnative *k = &natives[natives_n++];
        snprintf(k->cls, sizeof k->cls, "%s", c && c->cls ? c->cls : "?");
        snprintf(k->name, sizeof k->name, "%s", r[i].name);
        snprintf(k->sig, sizeof k->sig, "%s", r[i].sig);
        k->fn = r[i].fn;
        nx_log("RegisterNatives %s.%s%s -> %p", k->cls, k->name, k->sig, k->fn);
    }
    return 0;
}
static int32_t j_UnregisterNatives(void *e, jobj *c) { (void)e; (void)c; return 0; }

static void *j_FromReflected(void *e, void *o)
{
    (void)e;
    return o;
}
static void *j_ToReflected(void *e, jobj *c, void *m, uint8_t st)
{
    (void)e; (void)c; (void)st;
    return m;
}
static jobj *j_DefineClass(void *e, const char *n, void *l, const void *b, int32_t sz)
{
    (void)e; (void)l; (void)b; (void)sz;
    return mk_class(n ? n : "?");
}

/* --- JavaVM ----------------------------------------------------------- */

static int32_t vm_GetEnv(void *vm, void **out, int32_t ver)
{
    (void)vm; (void)ver;
    *out = tr_jni_env();
    return 0;
}
static int32_t vm_AttachCurrentThread(void *vm, void **out, void *args)
{
    (void)vm; (void)args;
    *out = tr_jni_env();
    return 0;
}
static int32_t vm_DetachCurrentThread(void *vm) { (void)vm; return 0; }
static int32_t vm_DestroyJavaVM(void *vm) { (void)vm; return 0; }

/* ------------------------------------------------------------------- build */

void tr_jni_init(void)
{
    for (int i = 0; i < JNI_SLOT_COUNT; i++)
        vt[i] = NULL;

    vt[JNI_GetVersion] = j_GetVersion;
    vt[JNI_DefineClass] = j_DefineClass;
    vt[JNI_FindClass] = j_FindClass;
    vt[JNI_FromReflectedMethod] = j_FromReflected;
    vt[JNI_FromReflectedField] = j_FromReflected;
    vt[JNI_ToReflectedMethod] = j_ToReflected;
    vt[JNI_GetSuperclass] = j_GetSuperclass;
    vt[JNI_IsAssignableFrom] = j_IsAssignableFrom;
    vt[JNI_ToReflectedField] = j_ToReflected;
    vt[JNI_Throw] = j_Throw;
    vt[JNI_ThrowNew] = j_ThrowNew;
    vt[JNI_ExceptionOccurred] = j_ExceptionOccurred;
    vt[JNI_ExceptionDescribe] = j_ExceptionDescribe;
    vt[JNI_ExceptionClear] = j_ExceptionClear;
    vt[JNI_FatalError] = j_FatalError;
    vt[JNI_PushLocalFrame] = j_PushLocalFrame;
    vt[JNI_PopLocalFrame] = j_PopLocalFrame;
    vt[JNI_NewGlobalRef] = j_NewGlobalRef;
    vt[JNI_DeleteGlobalRef] = j_DeleteRef;
    vt[JNI_DeleteLocalRef] = j_DeleteRef;
    vt[JNI_IsSameObject] = j_IsSameObject;
    vt[JNI_NewLocalRef] = j_NewLocalRef;
    vt[JNI_EnsureLocalCapacity] = j_EnsureLocalCapacity;
    vt[JNI_AllocObject] = j_AllocObject;
    vt[JNI_NewObject] = j_NewObject;
    vt[JNI_NewObjectV] = v_NewObject;
    vt[JNI_NewObjectA] = a_NewObject;
    vt[JNI_GetObjectClass] = j_GetObjectClass;
    vt[JNI_IsInstanceOf] = j_IsInstanceOf;
    vt[JNI_GetMethodID] = j_GetMethodID;

    /* Call<Type>Method / ...V / ...A, then the Nonvirtual and Static blocks:
     * same handlers, the dispatcher does not care which flavour was used. */
    static void *const call3[10][3] = {
        { c_obj, v_obj, a_obj },       { c_bool, v_bool, a_bool },
        { c_byte, v_byte, a_byte },    { c_char, v_char, a_char },
        { c_short, v_short, a_short }, { c_int, v_int, a_int },
        { c_long, v_long, a_long },    { c_float, v_float, a_float },
        { c_double, v_double, a_double }, { c_void, v_void, a_void },
    };
    for (int t = 0; t < 10; t++)
        for (int k = 0; k < 3; k++) {
            vt[JNI_CallObjectMethod + t * 3 + k] = call3[t][k];
            vt[JNI_CallNonvirtualObjectMethod + t * 3 + k] = call3[t][k];
            vt[JNI_CallStaticObjectMethod + t * 3 + k] = call3[t][k];
        }

    vt[JNI_GetFieldID] = j_GetFieldID;
    void *const getf[9] = { f_obj, f_bool, f_byte, f_char, f_short, f_int,
                            f_long, f_float, f_double };
    for (int i = 0; i < 9; i++) {
        vt[JNI_GetObjectField + i] = getf[i];
        vt[JNI_GetStaticObjectField + i] = getf[i];
        vt[JNI_SetObjectField + i] = f_set;
        vt[JNI_SetStaticObjectField + i] = f_set;
    }
    vt[JNI_GetStaticMethodID] = j_GetMethodID;
    vt[JNI_GetStaticFieldID] = j_GetFieldID;

    vt[JNI_NewString] = j_NewString;
    vt[JNI_GetStringLength] = j_GetStringLength;
    vt[JNI_GetStringChars] = j_GetStringChars;
    vt[JNI_ReleaseStringChars] = j_ReleaseStringChars;
    vt[JNI_NewStringUTF] = j_NewStringUTF;
    vt[JNI_GetStringUTFLength] = j_GetStringUTFLength;
    vt[JNI_GetStringUTFChars] = j_GetStringUTFChars;
    vt[JNI_ReleaseStringUTFChars] = j_ReleaseStringUTFChars;
    vt[JNI_GetStringRegion] = j_GetStringUTFRegion;
    vt[JNI_GetStringUTFRegion] = j_GetStringUTFRegion;
    vt[JNI_GetStringCritical] = j_GetStringChars;
    vt[JNI_ReleaseStringCritical] = j_ReleaseStringChars;

    vt[JNI_GetArrayLength] = j_GetArrayLength;
    vt[JNI_NewObjectArray] = j_NewObjectArray;
    vt[JNI_GetObjectArrayElement] = j_GetObjectArrayElement;
    vt[JNI_SetObjectArrayElement] = j_SetObjectArrayElement;
    for (int i = 0; i < 8; i++) {
        vt[JNI_NewBooleanArray + i] = j_NewByteArray;
        vt[JNI_GetBooleanArrayElements + i] = j_GetByteArrayElements;
        vt[JNI_ReleaseBooleanArrayElements + i] = j_ReleaseArrayElements;
        vt[JNI_GetBooleanArrayRegion + i] = j_GetByteArrayRegion;
        vt[JNI_SetBooleanArrayRegion + i] = j_SetByteArrayRegion;
    }
    /* Primitive-array slots are adjacent, but element width is not.  The
     * gamepad enumerator returns int[], so give that type its real byte stride. */
    vt[JNI_NewBooleanArray + 4] = j_NewIntArray;
    vt[JNI_GetBooleanArrayRegion + 4] = j_GetIntArrayRegion;
    vt[JNI_SetBooleanArrayRegion + 4] = j_SetIntArrayRegion;
    vt[JNI_GetPrimitiveArrayCritical] = j_GetPrimitiveArrayCritical;
    vt[JNI_ReleasePrimitiveArrayCritical] = j_ReleasePrimitiveArrayCritical;

    vt[JNI_RegisterNatives] = j_RegisterNatives;
    vt[JNI_UnregisterNatives] = j_UnregisterNatives;
    vt[JNI_MonitorEnter] = j_MonitorEnter;
    vt[JNI_MonitorExit] = j_MonitorExit;
    vt[JNI_GetJavaVM] = j_GetJavaVM;
    vt[JNI_NewWeakGlobalRef] = j_NewWeakGlobalRef;
    vt[JNI_DeleteWeakGlobalRef] = j_DeleteRef;
    vt[JNI_ExceptionCheck] = j_ExceptionCheck;
    vt[JNI_NewDirectByteBuffer] = j_NewDirectByteBuffer;
    vt[JNI_GetDirectBufferAddress] = j_GetDirectBufferAddress;
    vt[JNI_GetDirectBufferCapacity] = j_GetDirectBufferCapacity;
    vt[JNI_GetObjectRefType] = j_GetObjectRefType;

    jvm_vt[3] = vm_DestroyJavaVM;
    jvm_vt[4] = vm_AttachCurrentThread;
    jvm_vt[5] = vm_DetachCurrentThread;
    jvm_vt[6] = vm_GetEnv;
    jvm_vt[7] = vm_AttachCurrentThread;   /* AttachCurrentThreadAsDaemon */

    /* Bound up front rather than on demand so the VM's GetMethodID finds these
     * already carrying a handler; method_id matches on name and signature, so a
     * late bind would create a second, handler-less entry. */
    tr_jni_bind("java/lang/System", "nanoTime", "()J",
                 (void *)j_System_nanoTime);
    /* tr_jni_bind_ads() ficou DESLIGADO: medido, piorou muito -- o jogo passou
     * de solucos de 2,4s para 90s sem um unico frame.  Dizer "init terminou"
     * (isInitialized=1) faz a tela esperar um anuncio que nunca chega; o
     * default do dispatch (0 para tudo) e o que a deixa seguir.  Nao religar
     * sem antes provar, por frame, que nao e isso. */
    tr_jni_bind("java/lang/reflect/Method", "getName",
                 "()Ljava/lang/String;", (void *)j_Method_getName);

    input_device = mk_object("android/view/InputDevice");
    touch_device = mk_object("android/view/InputDevice");
    motion_range_list = mk_object("java/util/List");
    touch_motion_range_list = mk_object("java/util/List");
    motion_range_list->len = (int)(sizeof motion_ranges /
                                    sizeof *motion_ranges);
    motion_range_list->elems = calloc((size_t)motion_range_list->len,
                                      sizeof *motion_range_list->elems);
    for (int i = 0; i < motion_range_list->len; i++) {
        motion_ranges[i] = mk_object("android/view/InputDevice$MotionRange");
        motion_ranges[i]->prim = motion_axis_ids[i];
        motion_range_list->elems[i] = motion_ranges[i];
    }
    motion_range_iterator = mk_object("java/util/Iterator");
    key_event_object = mk_object("android/view/KeyEvent");
    motion_event_object = mk_object("android/view/MotionEvent");
    motion_event_object->data = &motion_event;
    preferences_object =
        mk_object("android/content/SharedPreferences");
    preferences_editor =
        mk_object("android/content/SharedPreferences$Editor");
    tr_jni_bind("android/view/InputDevice", "getDeviceIds", "()[I",
                 (void *)j_InputDevice_getDeviceIds);
    tr_jni_bind("android/view/InputDevice", "getDevice",
                 "(I)Landroid/view/InputDevice;",
                 (void *)j_InputDevice_getDevice);
    tr_jni_bind("android/view/InputEvent", "getDevice",
                 "()Landroid/view/InputDevice;",
                 (void *)j_InputEvent_getDevice);
    tr_jni_bind("android/view/InputDevice", "getName",
                 "()Ljava/lang/String;", (void *)j_InputDevice_getString);
    tr_jni_bind("android/view/InputDevice", "getDescriptor",
                 "()Ljava/lang/String;", (void *)j_InputDevice_getString);
    tr_jni_bind("android/view/InputDevice", "getVendorId", "()I",
                 (void *)j_InputDevice_getInt);
    tr_jni_bind("android/view/InputDevice", "getProductId", "()I",
                 (void *)j_InputDevice_getInt);
    tr_jni_bind("android/view/InputDevice", "getSources", "()I",
                 (void *)j_InputDevice_getInt);
    tr_jni_bind("android/view/InputDevice", "getId", "()I",
                 (void *)j_InputDevice_getInt);
    tr_jni_bind("android/view/InputDevice", "getControllerNumber", "()I",
                 (void *)j_InputDevice_getInt);
    tr_jni_bind("android/view/InputDevice", "getKeyboardType", "()I",
                 (void *)j_InputDevice_getInt);
    tr_jni_bind("android/view/InputDevice", "supportsSource", "(I)Z",
                 (void *)j_InputDevice_supportsSource);
    tr_jni_bind("android/view/InputDevice", "getMotionRanges",
                 "()Ljava/util/List;", (void *)j_InputDevice_getMotionRanges);
    tr_jni_bind("android/view/InputDevice", "getMotionRange",
                 "(I)Landroid/view/InputDevice$MotionRange;",
                 (void *)j_InputDevice_getMotionRange);
    tr_jni_bind("java/util/List", "size", "()I", (void *)j_List_size);
    tr_jni_bind("java/util/List", "get", "(I)Ljava/lang/Object;",
                 (void *)j_List_get);
    tr_jni_bind("java/util/List", "iterator", "()Ljava/util/Iterator;",
                 (void *)j_List_iterator);
    tr_jni_bind("java/util/Iterator", "hasNext", "()Z",
                 (void *)j_Iterator_hasNext);
    tr_jni_bind("java/util/Iterator", "next", "()Ljava/lang/Object;",
                 (void *)j_Iterator_next);
    tr_jni_bind("android/view/InputDevice$MotionRange", "getAxis", "()I",
                 (void *)j_MotionRange_getInt);
    tr_jni_bind("android/view/InputDevice$MotionRange", "getSource", "()I",
                 (void *)j_MotionRange_getInt);
    static const char *const range_float_methods[] = {
        "getMin", "getMax", "getFlat", "getFuzz", "getResolution",
    };
    for (size_t i = 0; i < sizeof range_float_methods /
                            sizeof *range_float_methods; i++)
        tr_jni_bind("android/view/InputDevice$MotionRange",
                     range_float_methods[i], "()F",
                     (void *)j_MotionRange_getFloat);

    static const char *const key_int_methods[] = {
        "getAction", "getKeyCode", "getSource", "getDeviceId",
        "getMetaState", "getRepeatCount", "getScanCode", "getFlags",
        "getUnicodeChar",
    };
    for (size_t i = 0; i < sizeof key_int_methods / sizeof *key_int_methods; i++)
        tr_jni_bind("android/view/KeyEvent", key_int_methods[i], "()I",
                     (void *)j_KeyEvent_getInt);
    tr_jni_bind("android/view/KeyEvent", "getEventTime", "()J",
                 (void *)j_KeyEvent_getLong);
    tr_jni_bind("android/view/KeyEvent", "getDownTime", "()J",
                 (void *)j_KeyEvent_getLong);
    tr_jni_bind("android/view/KeyEvent", "isSystem", "()Z",
                 (void *)j_KeyEvent_isSystem);

    static const char *const motion_int_methods[] = {
        "getAction", "getActionMasked", "getActionIndex", "getSource",
        "getDeviceId", "getMetaState", "getButtonState", "getFlags",
        "getPointerCount", "getHistorySize", "getEdgeFlags",
        "getActionButton", "getDisplayId", "getClassification",
    };
    for (size_t i = 0; i < sizeof motion_int_methods /
                            sizeof *motion_int_methods; i++)
        tr_jni_bind("android/view/MotionEvent", motion_int_methods[i],
                     "()I", (void *)j_MotionEvent_getInt);
    static const char *const motion_int_arg_methods[] = {
        "getPointerId", "findPointerIndex", "getToolType",
    };
    for (size_t i = 0; i < sizeof motion_int_arg_methods /
                            sizeof *motion_int_arg_methods; i++)
        tr_jni_bind("android/view/MotionEvent", motion_int_arg_methods[i],
                     "(I)I", (void *)j_MotionEvent_getInt);
    tr_jni_bind("android/view/MotionEvent", "getEventTime", "()J",
                 (void *)j_MotionEvent_getLong);
    tr_jni_bind("android/view/MotionEvent", "getDownTime", "()J",
                 (void *)j_MotionEvent_getLong);
    tr_jni_bind("android/view/MotionEvent", "getHistoricalEventTime", "(I)J",
                 (void *)j_MotionEvent_getLong);
    tr_jni_bind("android/view/MotionEvent", "getAxisValue", "(I)F",
                 (void *)j_MotionEvent_getFloat);
    tr_jni_bind("android/view/MotionEvent", "getAxisValue", "(II)F",
                 (void *)j_MotionEvent_getFloat);
    tr_jni_bind("android/view/MotionEvent", "getHistoricalAxisValue",
                 "(II)F", (void *)j_MotionEvent_getFloat);
    tr_jni_bind("android/view/MotionEvent", "getHistoricalAxisValue",
                 "(III)F", (void *)j_MotionEvent_getFloat);
    static const char *const motion_xy_methods[] = {
        "getX", "getY", "getRawX", "getRawY", "getPressure", "getSize",
        "getTouchMajor", "getTouchMinor", "getToolMajor", "getToolMinor",
        "getOrientation",
    };
    for (size_t i = 0; i < sizeof motion_xy_methods /
                            sizeof *motion_xy_methods; i++) {
        tr_jni_bind("android/view/MotionEvent", motion_xy_methods[i], "()F",
                     (void *)j_MotionEvent_getFloat);
        tr_jni_bind("android/view/MotionEvent", motion_xy_methods[i], "(I)F",
                     (void *)j_MotionEvent_getFloat);
    }
    tr_jni_bind("android/view/MotionEvent", "getHistoricalX", "(I)F",
                 (void *)j_MotionEvent_getFloat);
    tr_jni_bind("android/view/MotionEvent", "getHistoricalX", "(II)F",
                 (void *)j_MotionEvent_getFloat);
    tr_jni_bind("android/view/MotionEvent", "getHistoricalY", "(I)F",
                 (void *)j_MotionEvent_getFloat);
    tr_jni_bind("android/view/MotionEvent", "getHistoricalY", "(II)F",
                 (void *)j_MotionEvent_getFloat);
    tr_jni_bind("android/view/MotionEvent", "isFromSource", "(I)Z",
                 (void *)j_MotionEvent_isFromSource);
    tr_jni_bind("android/view/MotionEvent", "obtain",
                 "(Landroid/view/MotionEvent;)Landroid/view/MotionEvent;",
                 (void *)j_MotionEvent_obtain);
    tr_jni_bind("android/view/MotionEvent", "recycle", "()V",
                 (void *)j_MotionEvent_recycle);
    tr_jni_bind("android/view/MotionEvent", "getDevice",
                 "()Landroid/view/InputDevice;",
                 (void *)j_InputEvent_getDevice);
    tr_jni_bind("android/view/InputEvent", "getSource", "()I",
                 (void *)j_MotionEvent_getInt);
    tr_jni_bind("android/view/InputEvent", "getDeviceId", "()I",
                 (void *)j_MotionEvent_getInt);
    tr_jni_bind("android/view/InputEvent", "getEventTime", "()J",
                 (void *)j_MotionEvent_getLong);

    tr_jni_bind("java/io/File", "length", "()J", (void *)j_File_length);
    tr_jni_bind("java/io/File", "exists", "()Z", (void *)j_File_exists);
    tr_jni_bind("java/io/File", "getPath", "()Ljava/lang/String;",
                 (void *)j_File_getPath);
    tr_jni_bind("java/io/File", "getAbsolutePath", "()Ljava/lang/String;",
                 (void *)j_File_getPath);
    tr_jni_bind("java/io/File", "getCanonicalPath", "()Ljava/lang/String;",
                 (void *)j_File_getPath);
    tr_jni_bind("java/io/File", "getParent", "()Ljava/lang/String;",
                 (void *)j_File_getParent);
    tr_jni_bind("java/io/File", "getParentFile", "()Ljava/io/File;",
                 (void *)j_File_getParentFile);
    tr_jni_bind("java/io/File", "isDirectory", "()Z",
                 (void *)j_File_isDirectory);
    tr_jni_bind("java/io/File", "mkdirs", "()Z", (void *)j_File_mkdirs);

    tr_jni_bind("android/content/Context", "getFilesDir",
                 "()Ljava/io/File;", (void *)j_Context_getFilesDir);
    tr_jni_bind("android/content/Context", "getExternalFilesDir",
                 "(Ljava/lang/String;)Ljava/io/File;",
                 (void *)j_Context_getFilesDir);
    tr_jni_bind("android/content/Context", "getCacheDir",
                 "()Ljava/io/File;", (void *)j_Context_getFilesDir);
    tr_jni_bind("android/content/Context", "getPackageCodePath",
                 "()Ljava/lang/String;", (void *)j_Context_getPackageCodePath);
    tr_jni_bind("android/content/Context", "getPackageName",
                 "()Ljava/lang/String;", (void *)j_Context_getPackageName);
    tr_jni_bind("android/content/Context", "getAssets",
                 "()Landroid/content/res/AssetManager;",
                 (void *)j_Context_getAssets);
    /* initJni receives an Activity; Android inheritance makes these Context
     * methods, but the flat registry needs the concrete class aliases too. */
    tr_jni_bind("android/app/Activity", "getFilesDir",
                 "()Ljava/io/File;", (void *)j_Context_getFilesDir);
    tr_jni_bind("android/app/Activity", "getExternalFilesDir",
                 "(Ljava/lang/String;)Ljava/io/File;",
                 (void *)j_Context_getFilesDir);
    tr_jni_bind("android/app/Activity", "getPackageCodePath",
                 "()Ljava/lang/String;", (void *)j_Context_getPackageCodePath);
    tr_jni_bind("android/app/Activity", "getPackageName",
                 "()Ljava/lang/String;", (void *)j_Context_getPackageName);
    tr_jni_bind("android/app/Activity", "getAssets",
                 "()Landroid/content/res/AssetManager;",
                 (void *)j_Context_getAssets);
    tr_jni_bind("android/content/res/AssetManager", "open",
                "(Ljava/lang/String;)Ljava/io/InputStream;",
                (void *)j_AssetManager_open);
    tr_jni_bind("android/content/res/AssetManager", "open",
                "(Ljava/lang/String;I)Ljava/io/InputStream;",
                (void *)j_AssetManager_open);
    tr_jni_bind("android/content/res/AssetManager", "openFd",
                "(Ljava/lang/String;)Landroid/content/res/AssetFileDescriptor;",
                (void *)j_AssetManager_openFd);
    tr_jni_bind("android/content/res/AssetManager", "openNonAssetFd",
                "(Ljava/lang/String;)Landroid/content/res/AssetFileDescriptor;",
                (void *)j_AssetManager_openFd);
    tr_jni_bind("android/content/res/AssetFileDescriptor", "getStartOffset",
                "()J", (void *)j_afd_startOffset);
    tr_jni_bind("android/content/res/AssetFileDescriptor", "getDeclaredLength",
                "()J", (void *)j_afd_length);
    tr_jni_bind("android/content/res/AssetFileDescriptor", "getLength",
                "()J", (void *)j_afd_length);
    tr_jni_bind("android/content/res/AssetFileDescriptor",
                "getParcelFileDescriptor",
                "()Landroid/os/ParcelFileDescriptor;",
                (void *)j_afd_descriptor);
    tr_jni_bind("android/content/res/AssetFileDescriptor", "getFileDescriptor",
                "()Ljava/io/FileDescriptor;", (void *)j_afd_descriptor);
    tr_jni_bind("android/content/res/AssetFileDescriptor", "close", "()V",
                (void *)j_stream_close);
    tr_jni_bind("android/os/ParcelFileDescriptor", "getFd", "()I",
                (void *)j_fd_getFd);
    tr_jni_bind("android/os/ParcelFileDescriptor", "detachFd", "()I",
                (void *)j_fd_detachFd);
    tr_jni_bind("java/io/FileDescriptor", "descriptor", "I",
                (void *)j_fd_getFd);
    tr_jni_bind("java/io/InputStream", "available", "()I",
                (void *)j_stream_available);
    tr_jni_bind("java/io/InputStream", "skip", "(J)J",
                (void *)j_stream_skip);
    tr_jni_bind("java/io/InputStream", "close", "()V",
                (void *)j_stream_close);
    tr_jni_bind("java/io/InputStream", "read", "([B)I",
                (void *)j_stream_read);
    tr_jni_bind("java/io/InputStream", "read", "([BII)I",
                (void *)j_stream_read);
    tr_jni_bind("java/nio/channels/Channels", "newChannel",
                "(Ljava/io/InputStream;)Ljava/nio/channels/ReadableByteChannel;",
                (void *)j_Channels_newChannel);
    tr_jni_bind("java/nio/channels/ReadableByteChannel", "read",
                "(Ljava/nio/ByteBuffer;)I", (void *)j_channel_read);
    tr_jni_bind("java/io/InputStream", "read",
                "(Ljava/nio/ByteBuffer;)I", (void *)j_channel_read);
    tr_jni_bind("java/util/Scanner", "next", "()Ljava/lang/String;",
                 (void *)j_Scanner_next);
    tr_jni_bind("android/os/Environment", "getExternalStorageState",
                 "()Ljava/lang/String;", (void *)j_Environment_mounted);
    tr_jni_bind("android/os/Environment", "MEDIA_MOUNTED",
                 "Ljava/lang/String;", (void *)j_Environment_mounted);
    tr_jni_bind("java/lang/String", "equals", "(Ljava/lang/Object;)Z",
                 (void *)j_String_equals);
    tr_jni_bind("java/lang/StringBuilder", "toString",
                 "()Ljava/lang/String;", (void *)j_StringBuilder_toString);
    tr_jni_bind("android/net/Uri", "encode",
                 "(Ljava/lang/String;)Ljava/lang/String;",
                 (void *)j_Uri_encode);
    tr_jni_bind("android/net/Uri", "decode",
                 "(Ljava/lang/String;)Ljava/lang/String;",
                 (void *)j_Uri_decode);

    for (size_t i = 0; i < sizeof build_fields / sizeof *build_fields; i++) {
        tr_jni_bind("android/os/Build", build_fields[i].name,
                     "Ljava/lang/String;", (void *)j_Build_string);
        tr_jni_bind("android/os/Build$VERSION", build_fields[i].name,
                     "Ljava/lang/String;", (void *)j_Build_string);
    }
    tr_jni_bind("android/os/Build$VERSION", "SDK_INT", "I",
                 (void *)j_Build_SDK_INT);

    tr_jni_bind("android/app/Activity", "getPreferences",
                 "(I)Landroid/content/SharedPreferences;",
                 (void *)j_getPreferences);
    tr_jni_bind("android/content/Context", "getSharedPreferences",
                 "(Ljava/lang/String;I)Landroid/content/SharedPreferences;",
                 (void *)j_getPreferences);
    tr_jni_bind("android/content/SharedPreferences", "getBoolean",
                 "(Ljava/lang/String;Z)Z", (void *)j_Prefs_getBoolean);
    tr_jni_bind("android/content/SharedPreferences", "getInt",
                 "(Ljava/lang/String;I)I", (void *)j_Prefs_getInt);
    tr_jni_bind("android/content/SharedPreferences", "getFloat",
                 "(Ljava/lang/String;F)F", (void *)j_Prefs_getFloat);
    tr_jni_bind("android/content/SharedPreferences", "getString",
                 "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
                 (void *)j_Prefs_getString);
    tr_jni_bind("android/content/SharedPreferences", "contains",
                 "(Ljava/lang/String;)Z", (void *)j_Prefs_contains);
    tr_jni_bind("android/content/SharedPreferences", "edit",
                 "()Landroid/content/SharedPreferences$Editor;",
                 (void *)j_Prefs_edit);
    tr_jni_bind("android/content/SharedPreferences$Editor", "putInt",
                 "(Ljava/lang/String;I)"
                 "Landroid/content/SharedPreferences$Editor;",
                 (void *)j_Prefs_putInt);
    tr_jni_bind("android/content/SharedPreferences$Editor", "putFloat",
                 "(Ljava/lang/String;F)"
                 "Landroid/content/SharedPreferences$Editor;",
                 (void *)j_Prefs_putFloat);
    tr_jni_bind("android/content/SharedPreferences$Editor", "putString",
                 "(Ljava/lang/String;Ljava/lang/String;)"
                 "Landroid/content/SharedPreferences$Editor;",
                 (void *)j_Prefs_putString);
    tr_jni_bind("android/content/SharedPreferences$Editor", "putBoolean",
                 "(Ljava/lang/String;Z)"
                 "Landroid/content/SharedPreferences$Editor;",
                 (void *)j_Prefs_putBoolean);
    tr_jni_bind("android/content/SharedPreferences$Editor", "remove",
                 "(Ljava/lang/String;)"
                 "Landroid/content/SharedPreferences$Editor;",
                 (void *)j_Prefs_remove);
    tr_jni_bind("android/content/SharedPreferences$Editor", "clear",
                 "()Landroid/content/SharedPreferences$Editor;",
                 (void *)j_Prefs_clear);
    tr_jni_bind("android/content/SharedPreferences$Editor", "apply",
                 "()V", (void *)j_Prefs_apply);
    tr_jni_bind("android/content/SharedPreferences$Editor", "commit",
                 "()Z", (void *)j_Prefs_commit);
    tr_jni_bind("java/lang/Object", "getClass", "()Ljava/lang/Class;",
                 (void *)j_Object_getClass);
    tr_jni_bind("java/lang/Class", "getClassLoader",
                 "()Ljava/lang/ClassLoader;", (void *)j_getClassLoader);
    tr_jni_bind("java/lang/ClassLoader", "findLibrary",
                 "(Ljava/lang/String;)Ljava/lang/String;",
                 (void *)j_ClassLoader_findLibrary);
    tr_jni_bind("java/lang/System", "loadLibrary",
                 "(Ljava/lang/String;)V", (void *)j_System_loadLibrary);
    tr_jni_bind("java/lang/System", "load",
                 "(Ljava/lang/String;)V", (void *)j_System_loadLibrary);

    /* Sanity: the slot PairIP dereferences by offset must be NewStringUTF. */
    if (JNI_NewStringUTF * 8 != 1336)
        nx_die("JNIEnv layout drift: NewStringUTF is at %d, not 1336",
               JNI_NewStringUTF * 8);
    nx_log("jni: env=%p vtable slots=%d", tr_jni_env(), JNI_SLOT_COUNT);
}

void *tr_jni_sym(const char *name)
{
    (void)name;
    return NULL;
}
