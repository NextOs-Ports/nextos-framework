#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "il2.h"
#include "nx_elf.h"

typedef void Il2CppDomain;
typedef void Il2CppAssembly;
typedef void Il2CppImage;

static struct {
    int ready;
    Il2CppDomain *(*domain_get)(void);
    void *(*thread_attach)(Il2CppDomain *);
    const Il2CppAssembly **(*domain_get_assemblies)(const Il2CppDomain *,
                                                    size_t *);
    const Il2CppImage *(*assembly_get_image)(const Il2CppAssembly *);
    Il2CppClass *(*class_from_name)(const Il2CppImage *, const char *,
                                    const char *);
    FieldInfo *(*class_get_field_from_name)(Il2CppClass *, const char *);
    void (*field_static_get_value)(FieldInfo *, void *);
    void (*field_get_value)(Il2CppObject *, FieldInfo *, void *);
    void (*field_set_value)(Il2CppObject *, FieldInfo *, void *);
    Il2CppObject *(*runtime_invoke)(const MethodInfo *, void *, void **,
                                    Il2CppObject **);
    const MethodInfo *(*class_get_methods)(Il2CppClass *, void **);
    const char *(*method_get_name)(const MethodInfo *);
    uint32_t (*method_get_param_count)(const MethodInfo *);
    uint8_t (*method_is_generic)(const MethodInfo *);
    void *(*object_unbox)(Il2CppObject *);
    Il2CppClass *(*object_get_class)(Il2CppObject *);
    const char *(*class_get_name)(Il2CppClass *);
    void *(*class_get_type)(Il2CppClass *);
    Il2CppObject *(*type_get_object)(void *);
    Il2CppObject *(*string_new)(const char *);
    const void *(*method_get_param)(const MethodInfo *, uint32_t);
    Il2CppClass *(*class_from_il2cpp_type)(const void *);
    uint32_t (*gchandle_new)(Il2CppObject *, int);
    Il2CppClass *(*class_get_parent)(Il2CppClass *);
} il2;

int il2_load(void)
{
    if (il2.ready)
        return il2.ready > 0;
    nx_mod *m = nx_find_mod("libil2cpp.so");
    if (!m)
        return 0;
    il2.ready = -1;
#define GET(field, name) \
    do { *(void **)&il2.field = nx_lookup_in(m, name); \
         if (!il2.field) { nx_log("il2: falta %s", name); return 0; } } while (0)
    GET(domain_get, "il2cpp_domain_get");
    GET(thread_attach, "il2cpp_thread_attach");
    GET(domain_get_assemblies, "il2cpp_domain_get_assemblies");
    GET(assembly_get_image, "il2cpp_assembly_get_image");
    GET(class_from_name, "il2cpp_class_from_name");
    GET(class_get_field_from_name, "il2cpp_class_get_field_from_name");
    GET(field_static_get_value, "il2cpp_field_static_get_value");
    GET(field_get_value, "il2cpp_field_get_value");
    GET(field_set_value, "il2cpp_field_set_value");
    GET(runtime_invoke, "il2cpp_runtime_invoke");
    GET(class_get_methods, "il2cpp_class_get_methods");
    GET(method_get_name, "il2cpp_method_get_name");
    GET(method_get_param_count, "il2cpp_method_get_param_count");
    GET(method_is_generic, "il2cpp_method_is_generic");
    GET(object_unbox, "il2cpp_object_unbox");
    GET(object_get_class, "il2cpp_object_get_class");
    GET(class_get_name, "il2cpp_class_get_name");
    GET(class_get_type, "il2cpp_class_get_type");
    GET(type_get_object, "il2cpp_type_get_object");
    GET(string_new, "il2cpp_string_new");
    GET(method_get_param, "il2cpp_method_get_param");
    GET(class_from_il2cpp_type, "il2cpp_class_from_il2cpp_type");
    GET(gchandle_new, "il2cpp_gchandle_new");
    GET(class_get_parent, "il2cpp_class_get_parent");
#undef GET
    il2.ready = 1;
    return 1;
}

void il2_attach(void)
{
    Il2CppDomain *d = il2.domain_get ? il2.domain_get() : NULL;
    if (d)
        il2.thread_attach(d);
}

Il2CppClass *il2_class(const char *ns, const char *name)
{
    Il2CppDomain *domain = il2.domain_get();
    if (!domain)
        return NULL;
    size_t n = 0;
    const Il2CppAssembly **as = il2.domain_get_assemblies(domain, &n);
    for (size_t i = 0; i < n; i++) {
        const Il2CppImage *img = il2.assembly_get_image(as[i]);
        if (!img)
            continue;
        Il2CppClass *k = il2.class_from_name(img, ns, name);
        if (k)
            return k;
    }
    nx_log("il2: classe %s.%s ausente", ns, name);
    return NULL;
}

Il2CppClass *il2_class_of(Il2CppObject *obj)
{
    return obj ? il2.object_get_class(obj) : NULL;
}

const char *il2_class_name(Il2CppClass *k)
{
    return k ? il2.class_get_name(k) : "(null)";
}

/* O IL2CPP remove sobrecargas nao usadas; escolher por nome+aridade MEDIDOS
 * (licao do GetComponentInParent), pulando definicoes genericas, que tem a
 * mesma aridade mas nao podem ser invocadas sem inflar. */
const MethodInfo *il2_method(Il2CppClass *k, const char *name, unsigned argc)
{
    if (!k)
        return NULL;
    void *iter = NULL;
    const MethodInfo *m;
    while ((m = il2.class_get_methods(k, &iter))) {
        const char *n = il2.method_get_name(m);
        if (n && strcmp(n, name) == 0 &&
            il2.method_get_param_count(m) == argc &&
            !il2.method_is_generic(m))
            return m;
    }
    nx_log("il2: metodo %s/%u ausente em %s", name, argc,
           il2.class_get_name(k));
    return NULL;
}

/* Mesma coisa, desempatando sobrecargas pelo nome da classe do 1o parametro
 * ("String", "Int32"...). */
const MethodInfo *il2_method_p(Il2CppClass *k, const char *name, unsigned argc,
                               const char *param0_class)
{
    if (!k)
        return NULL;
    void *iter = NULL;
    const MethodInfo *m;
    while ((m = il2.class_get_methods(k, &iter))) {
        const char *n = il2.method_get_name(m);
        if (!n || strcmp(n, name) != 0 ||
            il2.method_get_param_count(m) != argc || il2.method_is_generic(m))
            continue;
        if (argc == 0)
            return m;
        Il2CppClass *pk = il2.class_from_il2cpp_type(il2.method_get_param(m, 0));
        if (pk && strcmp(il2.class_get_name(pk), param0_class) == 0)
            return m;
    }
    nx_log("il2: metodo %s/%u(%s) ausente em %s", name, argc, param0_class,
           il2.class_get_name(k));
    return NULL;
}

Il2CppObject *il2_call(const MethodInfo *m, void *self, void **args,
                       const char *what)
{
    if (!m)
        return NULL;
    Il2CppObject *ex = NULL;
    Il2CppObject *r = il2.runtime_invoke(m, self, args, &ex);
    if (ex) {
        nx_log("il2: %s lancou %s", what,
               il2.class_get_name(il2.object_get_class(ex)));
        return NULL;
    }
    return r;
}

FieldInfo *il2_field(Il2CppClass *k, const char *name)
{
    FieldInfo *f = k ? il2.class_get_field_from_name(k, name) : NULL;
    if (!f)
        nx_log("il2: campo %s ausente em %s", name, il2_class_name(k));
    return f;
}

void il2_field_get(Il2CppObject *obj, FieldInfo *f, void *out)
{
    il2.field_get_value(obj, f, out);
}

void il2_field_set(Il2CppObject *obj, FieldInfo *f, void *value)
{
    il2.field_set_value(obj, f, value);
}

void il2_static_get(FieldInfo *f, void *out)
{
    il2.field_static_get_value(f, out);
}

void *il2_unbox(Il2CppObject *boxed)
{
    return boxed ? il2.object_unbox(boxed) : NULL;
}

Il2CppObject *il2_type(Il2CppClass *k)
{
    return k ? il2.type_get_object(il2.class_get_type(k)) : NULL;
}

/* Cabecalho de array do il2cpp (32 bits): klass, monitor, bounds, max_length,
 * e so' entao os elementos. */
uint32_t il2_arr_len(Il2CppObject *arr)
{
    return arr ? *(uint32_t *)((char *)arr + 3 * sizeof(void *)) : 0;
}

void *il2_arr_at(Il2CppObject *arr, uint32_t i)
{
    return *(void **)((char *)arr + 4 * sizeof(void *) + i * sizeof(void *));
}

/* String do il2cpp: klass, monitor, length(int32), chars UTF-16. */
const char *il2_str_utf8(Il2CppObject *s, char *out, size_t size)
{
    if (!s) {
        snprintf(out, size, "(null)");
        return out;
    }
    int32_t len = *(int32_t *)((char *)s + 2 * sizeof(void *));
    const uint16_t *ch = (const uint16_t *)((char *)s + 2 * sizeof(void *) + 4);
    size_t o = 0;
    for (int32_t i = 0; i < len && o + 1 < size; i++)
        out[o++] = ch[i] < 0x80 ? (char)ch[i] : '?';
    out[o] = 0;
    return out;
}

Il2CppObject *il2_str_new(const char *utf8)
{
    return il2.string_new(utf8);
}

/* Segura o objeto contra o GC (o ponteiro nativo sozinho nao conta como raiz). */
void il2_pin(Il2CppObject *obj)
{
    if (obj)
        il2.gchandle_new(obj, 1);
}

/* Properties declared on a base class (GraphOwner.graph, MonoBehaviour.enabled)
 * are not in the derived class's own method list, and asking only the derived
 * class reports them as "missing" -- the same shape of mistake as picking a
 * stripped overload.  Walk the inheritance chain instead. */
const MethodInfo *il2_method_deep(Il2CppClass *k, const char *name,
                                  unsigned argc)
{
    for (Il2CppClass *c = k; c; c = il2.class_get_parent(c)) {
        void *iter = NULL;
        const MethodInfo *m;
        while ((m = il2.class_get_methods(c, &iter))) {
            const char *n = il2.method_get_name(m);
            if (n && strcmp(n, name) == 0 &&
                il2.method_get_param_count(m) == argc &&
                !il2.method_is_generic(m))
                return m;
        }
    }
    nx_log("il2: metodo %s/%u ausente na cadeia de %s", name, argc,
           il2_class_name(k));
    return NULL;
}

Il2CppClass *il2_class_parent(Il2CppClass *k)
{
    return k ? il2.class_get_parent(k) : NULL;
}
