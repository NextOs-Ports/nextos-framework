/*
 * protected.c -- the two mechanical operations PairIP performs, done natively.
 *
 * Both game objects ship 48 KB of encrypted .text, libApplicationMain a second
 * encrypted 48 KB window in .data, and both have PLT slots whose relocation
 * records were removed.  Measured on the file, the encrypted window is a
 * carrier -- 84 real functions and 345 direct calls into it -- so there is
 * nothing to stub: the code has to be plaintext to run at all.
 *
 * On Android those two things are done by PairIP's own interpreter, which
 * emulates Java bytecode inside a Dalvik process.  This port does not have one
 * and does not reproduce one: it restores the same two results from
 * version-pinned, user-generated data --
 *
 *   - three plaintext windows, hashed so a wrong or edited file fails loudly;
 *   - the slot -> symbol map in tr_plt_names.h, which names every hidden PLT
 *     entry so it can be filled from the port's own import tables.
 *
 * Production therefore never loads libpairipcore, never runs the interpreter
 * and never contacts a licence service.  TR_VM_CAPTURE still loads it, because
 * that is the mode that produced this data from the user's own copy.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nx_elf.h"
#include "tr.h"
#include "tr_plt_names.h"

int tr_trace_plt;

/* The asset name is StartupLauncher.startupProgramName in this build. */
#define STARTUP_PROGRAM "9t5FrB6yUInYdii5"

/* ---------------------------------------------- the interpreter, capture only */

/* ExecuteProgram is imported by both game objects.  In capture mode it goes to
 * the real interpreter; in production the objects' DT_INIT is never run, so
 * nothing calls it -- but the symbol still has to resolve for the mapped image
 * to be internally complete. */
static void *execute_program(const char *name, void **args, long nargs)
{
    static void *(*real)(const char *, void **, long);
    if (!real) {
        nx_mod *pairip = nx_find_mod("libpairipcore.so");
        real = pairip ? (void *(*)(const char *, void **, long))
                 nx_lookup_in(pairip, "ExecuteProgram") : NULL;
    }
    if (!real) {
        nx_log("ExecuteProgram(%s) with no interpreter loaded",
               name ? name : "?");
        return NULL;
    }
    nx_log("ExecuteProgram(%s, %ld)", name ? name : "?", nargs);
    return real(name, args, nargs);
}

void *tr_protected_sym(const char *name)
{
    if (name && strcmp(name, "ExecuteProgram") == 0)
        return (void *)(uintptr_t)execute_program;
    return NULL;
}

static unsigned char *read_whole_file(const char *path, size_t *out_len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return NULL;
    }
    unsigned char *buf = malloc((size_t)st.st_size);
    size_t done = 0;
    while (buf && done < (size_t)st.st_size) {
        ssize_t got = read(fd, buf + done, (size_t)st.st_size - done);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            break;
        done += (size_t)got;
    }
    close(fd);
    if (!buf || done != (size_t)st.st_size) {
        free(buf);
        return NULL;
    }
    *out_len = done;
    return buf;
}

static unsigned char *read_asset(const char *rel, size_t *out_len)
{
    char path[1280];
    snprintf(path, sizeof path, "%s/%s", tr_datadir, rel);
    unsigned char *d = read_whole_file(path, out_len);
    if (!d)
        fprintf(stderr, "[tr] cannot read VM program %s\n", path);
    return d;
}

/* VMRunner.invoke(name, args): read the named program out of the APK assets
 * and hand it to the interpreter.
 *
 * ExecuteProgram does not carry bytecode -- it bounces up to Java for it,
 * exactly as VMRunner.invoke does on Android: readByteCode(name) from the base
 * APK, then the native executeVM(bytes, args).  Capture mode needs this;
 * production never reaches it. */
static int64_t j_VMRunner_invoke(jctx *c)
{
    const char *name = tr_jarg_str(c);
    void *args = tr_jarg_obj(c);
    if (!name || !*name)
        return 0;
    void *fn = tr_jni_native("com/pairip/VMRunner", "executeVM");
    if (!fn) {
        fprintf(stderr, "[tr] VMRunner.invoke(%s): executeVM is not "
                        "registered\n", name);
        return 0;
    }
    size_t len = 0;
    unsigned char *code = read_asset(name, &len);
    if (!code)
        return 0;
    void *bytes = tr_jret_bytes(code, (int)len);
    free(code);
    nx_log("VMRunner.invoke(%s): %zu bytes of bytecode", name, len);
    void *r = ((void *(*)(void *, void *, void *, void *))fn)(
        tr_jni_env(), tr_jret_class("com/pairip/VMRunner"), bytes, args);
    return (int64_t)(uintptr_t)r;
}

static int64_t j_VMRunner_getContext(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)tr_jret_obj("android/app/Activity");
}

void tr_jni_bind_pairip(void)
{
    tr_jni_bind("com/pairip/VMRunner", "invoke",
                "(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/Object;",
                (void *)j_VMRunner_invoke);
    tr_jni_bind("com/pairip/VMRunner", "getContext",
                "()Landroid/content/Context;",
                (void *)j_VMRunner_getContext);
}

/* StartupLauncher.launch(), natively.  Capture mode only. */
int tr_pairip_startup(void)
{
    void *fn = tr_jni_native("com/pairip/VMRunner", "executeVM");
    if (!fn) {
        fprintf(stderr, "[tr] libpairipcore did not register executeVM\n");
        return -1;
    }
    size_t len = 0;
    unsigned char *code = read_asset(STARTUP_PROGRAM, &len);
    if (!code)
        return -1;
    void *bytes = tr_jret_bytes(code, (int)len);
    free(code);
    fprintf(stderr, "[tr] pairip startup program %s (%zu bytes)\n",
            STARTUP_PROGRAM, len);
    void *r = ((void *(*)(void *, void *, void *, void *))fn)(
        tr_jni_env(), tr_jret_class("com/pairip/VMRunner"), bytes, NULL);
    fprintf(stderr, "[tr] pairip startup returned %p\n", r);
    return 0;
}

/* ------------------------------------------------ the transformation masks */

/* One mask per protected object.  A mask is NOT the game's code: it holds
 * `plaintext XOR ciphertext`, so on its own it is a difference and reveals
 * nothing.  It only becomes anything when XORed over the very bytes the user's
 * own copy of the library already contains -- which is what makes a BYO-data
 * package possible, and is the same shape Prizefighters 2 ships.
 *
 * The bytes it covers are only those PairIP writes that this loader cannot
 * compute itself: every relocation-covered word and every donor address was
 * excluded when the mask was built, so applying it after relocation cannot
 * undo an address this loader computed.
 *
 * Format: "NXMK", u32 count, then {u64 vaddr, u32 len, bytes} records.
 * `plain_sha256` is the SHA-256 of the reconstructed plaintext: checking it
 * both proves the mask was applied correctly and proves the user's APK is the
 * supported build -- a different build fails here with a clear message instead
 * of crashing later. */
typedef struct {
    const char *soname;
    const char *file;
    uint64_t plain_fnv1a;
} overlay;

static const overlay overlays[] = {
    { "liblime.so",            "liblime.xormask",
      UINT64_C(0xe1e5f83fab06f194) },
    { "libApplicationMain.so", "libApplicationMain.xormask",
      UINT64_C(0xabd7882404fb58af) },
};

static inline uint64_t fnv1a_step(uint64_t h, unsigned char byte)
{
    h ^= byte;
    return h * UINT64_C(0x100000001b3);
}

int tr_apply_text_overlays(void)
{
    char path[1280];
    for (size_t i = 0; i < sizeof overlays / sizeof *overlays; i++) {
        const overlay *o = &overlays[i];
        nx_mod *m = nx_find_mod(o->soname);
        if (!m)
            nx_die("cannot restore %s: %s is not loaded", o->file, o->soname);

        snprintf(path, sizeof path, "%s/tools/patches/%s",
                 tr_gamedir, o->file);
        size_t len = 0;
        unsigned char *buf = read_whole_file(path, &len);
        if (!buf)
            nx_die("missing %s -- the package is incomplete", path);
        if (len < 8 || memcmp(buf, "NXMK", 4) != 0)
            nx_die("%s is not a transformation mask", path);

        uint32_t count;
        memcpy(&count, buf + 4, sizeof count);
        size_t at = 8, bytes = 0;
        uint64_t plain = UINT64_C(0xcbf29ce484222325);
        for (uint32_t k = 0; k < count; k++) {
            if (at + 12 > len)
                nx_die("%s is truncated at record %u", path, k);
            uint64_t vaddr;
            uint32_t n;
            memcpy(&vaddr, buf + at, sizeof vaddr);
            memcpy(&n, buf + at + 8, sizeof n);
            at += 12;
            if (at + n > len)
                nx_die("%s record %u runs past the end", path, k);
            if (vaddr > m->span || n > m->span - vaddr)
                nx_die("%s record %u is outside %s", path, k, o->soname);
            /* plaintext = ciphertext (o que a lib DO USUARIO ja tem em
             * memoria) XOR mascara.  Nada de codigo do jogo foi distribuido:
             * a mascara so vale sobre esses bytes. */
            for (uint32_t b = 0; b < n; b++) {
                unsigned char v = m->base[vaddr + b] ^ buf[at + b];
                m->base[vaddr + b] = v;
                plain = fnv1a_step(plain, v);
            }
            /* Per range, not per image: the reserved span has PROT_NONE gaps
             * between segments, and a cache maintenance instruction on one of
             * them faults. */
            __builtin___clear_cache((char *)m->base + vaddr,
                                    (char *)m->base + vaddr + n);
            at += n;
            bytes += n;
        }
        free(buf);
        /* Confere o texto claro RECONSTRUIDO.  Isso prova as duas coisas de
         * uma vez: a mascara foi aplicada certo, e o APK do usuario e' o build
         * suportado.  Outro build falha AQUI, com mensagem clara, em vez de
         * quebrar mais adiante sem explicacao. */
        if (plain != o->plain_fnv1a)
            nx_die("%s: o conteudo reconstruido nao confere (FNV-1a %016llx, "
                   "esperado %016llx) -- provavelmente outra build do APK",
                   o->soname, (unsigned long long)plain,
                   (unsigned long long)o->plain_fnv1a);
        fprintf(stderr, "[tr] restored %s: %u ranges, %zu bytes\n",
                o->soname, count, bytes);
    }
    return 0;
}

/* ------------------------------------------------------- the hidden PLT slots */

/* Version-pinned for this build: .plt is 0x18b0 and 0xa20 bytes of 16-byte
 * entries, so 393 and 161 real entries after the shared PLT0 header at
 * plt_addr.  Both objects are BIND_NOW, so a slot still holding plt_addr was
 * never relocated -- calling it lands on a lazy resolver that under BIND_NOW
 * was never set up, which is the fault every unfilled slot produces. */
static const struct {
    const char *soname;
    uintptr_t plt_addr;
    size_t plt_entries;
    const tr_plt_ent *names;
    size_t names_n;
} PROTECTED[] = {
    { "liblime.so", 0x9df50, 393,
      tr_liblime_plt, sizeof tr_liblime_plt / sizeof *tr_liblime_plt },
    { "libApplicationMain.so", 0x305e90, 161,
      tr_libapplicationmain_plt,
      sizeof tr_libapplicationmain_plt / sizeof *tr_libapplicationmain_plt },
};

static void *impl_of(const char *name)
{
    void *fn = nx_resolve_import(name);
    if (!fn)
        fn = tr_android_sym(name);
    if (!fn)
        fn = tr_egl_sym(name);          /* also answers for every gl* name */
    if (!fn)
        fn = tr_audio_sym(name);
    if (!fn)
        fn = nx_lookup(name);
    return fn;
}

/* Reached only if the game calls a PLT slot the recovered map does not name.
 * It reports the slot and stops, because continuing would run whatever the
 * argument registers happened to point at. */
static void unnamed_slot_called(void)
{
    nx_die("called PLT slot %u, which the recovered map does not name",
           nx_probe_slot);
}

int tr_patch_protected_plt(void)
{
    int missing = 0;
    for (size_t i = 0; i < sizeof PROTECTED / sizeof *PROTECTED; i++) {
        nx_mod *m = nx_find_mod(PROTECTED[i].soname);
        if (!m || !m->pltgot)
            continue;

        int filled = 0, already = 0, unnamed = 0;
        for (size_t k = 0; k < PROTECTED[i].names_n; k++) {
            unsigned slot = PROTECTED[i].names[k].slot;
            const char *name = PROTECTED[i].names[k].name;
            /* A slot the ordinary relocation pass already resolved is left
             * alone: its own .rela.plt entry is the authority. */
            if (m->pltgot[slot] != PROTECTED[i].plt_addr) {
                already++;
                continue;
            }
            if (!name) {
                /* Zero on Android too: nothing calls through this slot. */
                m->pltgot[slot] = 0;
                filled++;
                continue;
            }
            void *fn = impl_of(name);
            if (!fn) {
                missing++;
                fprintf(stderr, "[tr] %s PLT %u: no implementation for %s\n",
                        PROTECTED[i].soname, slot, name);
                continue;
            }
            m->pltgot[slot] = (uintptr_t)fn;
            filled++;
            if (tr_trace_plt)
                nx_log("%s PLT %u <- %s (%p)", PROTECTED[i].soname, slot,
                       name, fn);
        }
        /* Anything still holding the link-time PLT0 has no name in the map.
         * Rather than let it jump to a resolver that does not exist, point it
         * at a stub that says which slot it was: that turns a silent fault
         * into a line naming exactly what is missing. */
        for (size_t s = 3; s < 3 + PROTECTED[i].plt_entries; s++) {
            if (m->pltgot[s] != PROTECTED[i].plt_addr)
                continue;
            m->pltgot[s] = (uintptr_t)nx_make_probe((unsigned)s,
                                                   (void *)unnamed_slot_called);
            unnamed++;
        }

        fprintf(stderr, "[tr] %s PLT: %d filled, %d already relocated, "
                        "%d unnamed (trapped)\n",
                PROTECTED[i].soname, filled, already, unnamed);
    }
    return missing;
}
