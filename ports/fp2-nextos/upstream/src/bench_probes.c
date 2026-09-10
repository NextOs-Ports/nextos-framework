/* Sondas de bancada (FP2_INPUT_DIAG=1 na variante -DFP2_BENCH_PROBES).
 * 1) bindings REAIS do jogo: InputControl.mKeysList -> KeyMapping{mName,
 *    mPrimaryInput..}; JoystickInput{mButton,mAxis,mTarget} (0-based).
 * 2) ponteiros de métodos (offset no libil2cpp.so) para desmontagem offline:
 *    quem monta o texto dos prompts ("Button N", "Axis N (+)"). */
#ifdef FP2_BENCH_PROBES
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "il2.h"
#include "nx_elf.h"
#include "bench_probes.h"

static int dumped;

static void dump_bindings(void)
{
    Il2CppClass *ic = il2_class("", "InputControl");
    FieldInfo *lf = ic ? il2_field(ic, "mKeysList") : NULL;
    Il2CppObject *list = NULL;
    if (lf) il2_static_get(lf, &list);
    if (!list) { fprintf(stderr, "[fp2/diag] InputControl.mKeysList indisponível\n"); return; }
    Il2CppClass *lk = il2_class_of(list);
    FieldInfo *items_f = lk ? il2_field(lk, "_items") : NULL;
    FieldInfo *size_f = lk ? il2_field(lk, "_size") : NULL;
    Il2CppObject *items = NULL; int32_t size = 0;
    if (items_f) il2_field_get(list, items_f, &items);
    if (size_f) il2_field_get(list, size_f, &size);
    fprintf(stderr, "[fp2/diag] game bindings: %d KeyMapping\n", size);
    for (int32_t i = 0; i < size && items; i++) {
        Il2CppObject *km = il2_arr_at(items, (uint32_t)i);
        if (!km) continue;
        Il2CppClass *kc = il2_class_of(km);
        char name[64] = "?";
        FieldInfo *nf = il2_field(kc, "mName");
        Il2CppObject *ns = NULL; if (nf) il2_field_get(km, nf, &ns);
        if (ns) il2_str_utf8(ns, name, sizeof name);
        fprintf(stderr, "[fp2/diag]  %-14s", name);
        const char *slots[3] = { "mPrimaryInput", "mSecondaryInput", "mThirdInput" };
        for (int s = 0; s < 3; s++) {
            FieldInfo *sf = il2_field(kc, slots[s]);
            Il2CppObject *in = NULL; if (sf) il2_field_get(km, sf, &in);
            if (!in) { fprintf(stderr, " | -"); continue; }
            Il2CppClass *cc = il2_class_of(in);
            const char *cn = il2_class_name(cc);
            if (cn && strcmp(cn, "JoystickInput") == 0) {
                int32_t btn = -1, axis = -1, target = -1;
                FieldInfo *f;
                if ((f = il2_field(cc, "mButton"))) il2_field_get(in, f, &btn);
                if ((f = il2_field(cc, "mAxis"))) il2_field_get(in, f, &axis);
                if ((f = il2_field(cc, "mTarget"))) il2_field_get(in, f, &target);
                fprintf(stderr, " | Joystick button=%d axis=%d target=%d", btn, axis, target);
            } else if (cn && strcmp(cn, "KeyboardInput") == 0) {
                int32_t key = -1; FieldInfo *f = il2_field(cc, "mKey");
                if (f) il2_field_get(in, f, &key);
                fprintf(stderr, " | Key %d", key);
            } else fprintf(stderr, " | %s", cn ? cn : "?");
        }
        fprintf(stderr, "\n");
    }
}

static void dump_method_pointer(nx_mod *mod, const char *ns, const char *cls,
                                const char *method, int argc)
{
    Il2CppClass *k = il2_class(ns, cls);
    const MethodInfo *m = k ? il2_method_deep(k, method, (unsigned)argc) : NULL;
    if (!m) { fprintf(stderr, "[fp2/diag] method %s.%s/%d: não resolvido\n", cls, method, argc); return; }
    /* MethodInfo: o primeiro campo é o methodPointer (ABI do il2cpp). */
    uintptr_t ptr = *(const uintptr_t *)m;
    fprintf(stderr, "[fp2/diag] method %s.%s/%d: libil2cpp+0x%lx\n", cls, method, argc,
            mod ? (unsigned long)(ptr - (uintptr_t)mod->base) : (unsigned long)ptr);
}


/* Experimento de bancada: FPSaveManager.buttonType (estático, byte) escolhe o
 * estilo dos ícones de botão do jogo. Um número em /tmp/fp2-btype é aplicado
 * a cada 30 quadros para fotografar cada estilo. Só na bancada. */
static void apply_button_type(unsigned long frame)
{
    static int last = -1;
    if (frame % 30 != 7) return;
    FILE *f = fopen("/tmp/fp2-btype", "r");
    if (!f) return;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    if (v < 0 || v > 15) return;
    Il2CppClass *sm = il2_class("", "FPSaveManager");
    FieldInfo *bt = sm ? il2_field(sm, "buttonType") : NULL;
    nx_mod *mod = nx_find_mod("libil2cpp.so");
    void (*static_set)(void *, void *) = mod ? (void *)nx_lookup_in(mod, "il2cpp_field_static_set_value") : NULL;
    if (!bt || !static_set) { if (last != -2) fprintf(stderr, "[fp2/diag] buttonType: campo/setter indisponível\n"); last = -2; return; }
    uint8_t b = (uint8_t)v;
    static_set(bt, &b);
    if (v != last) { fprintf(stderr, "[fp2/diag] FPSaveManager.buttonType := %d\n", v); last = v; }
}

void fp2_bench_probes(unsigned long frame)
{
    if (getenv("FP2_INPUT_DIAG") && il2_load()) { il2_attach(); apply_button_type(frame); }
    if (dumped || frame < 120 || !getenv("FP2_INPUT_DIAG") || !il2_load())
        return;
    dumped = 1;
    il2_attach();
    dump_bindings();
    nx_mod *mod = nx_find_mod("libil2cpp.so");
    struct { const char *cls, *m; int argc; } probes[] = {
        { "FPStage", "ButtonGraphic", 1 }, { "FPStage", "ButtonGraphic", 2 },
        { "JoystickInput", "ToString", 0 }, { "JoystickInput", "getInputName", 0 },
        { "TutorialTV", "Start", 0 }, { "MenuControls", "Start", 0 },
        { "FPHudMaster", "GuideUpdate", 0 }, { "InputControl", "GetJoystickNames", 0 },
    };
    for (size_t i = 0; i < sizeof probes / sizeof *probes; i++)
        dump_method_pointer(mod, "", probes[i].cls, probes[i].m, probes[i].argc);
}
#endif /* FP2_BENCH_PROBES */
