/*
 * protected.c -- native replacement for the APK's PairIP bootstrap.
 *
 * The Android wrapper normally asks PairIP to do two mechanical operations:
 * restore one protected .text range in libunity/libil2cpp and fill PLT slots
 * whose relocation records were removed.  The NextOS port performs exactly
 * those two operations from version-pinned, user-supplied arm64 data.  It does
 * not load libpairipcore, run its VM, contact a licence service, or emulate an
 * Android application.
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
#include "pf2.h"
#include "pf2_unity_imports.h"
#include "pf2_il2cpp_imports.h"
#include "pf2_captured_imports.h"

int pf2_trace_plt;

typedef struct {
    const char *soname;
    const char *file;
    uintptr_t vaddr;
    size_t size;
    uint64_t fnv1a;
} text_overlay;

/* Exact protected ranges from Prizefighters 2 v1.09.3 arm64-v8a.
 *
 * PairIP spends its encryption budget twice per library: once at the start of
 * .text and once on a window of writable data.  In libil2cpp the data window is
 * the IL2CPP metadata-usage table, so without it the first token the scripting
 * runtime reads is garbage and it indexes metadata far out of bounds.
 *
 * Order matters: these are applied after the modules are relocated, so a window
 * must cover only the encrypted range and nothing around it.  The bytes on
 * either side hold pointers that our own R_AARCH64_RELATIVE processing has
 * already filled in, and overwriting them with the addresses a different
 * process happened to have would undo that.
 */
static const text_overlay overlays[] = {
    { "libil2cpp.so", "libil2cpp.text", 0x0118bf7c, 0x0000c800,
      UINT64_C(0xe180f7edacc87c12) },
    { "libunity.so",  "libunity.text",  0x00397710, 0x0000c800,
      UINT64_C(0xa6a5cc5e61acca0b) },
    { "libil2cpp.so", "libil2cpp.data", 0x02f0e7a0, 0x0000c7b0,
      UINT64_C(0xb4933370a4e65e14) },
    { "libunity.so",  "libunity.data",  0x011cdbf8, 0x00004810,
      UINT64_C(0x1aba990bc9f7bdd2) },
};

/*
 * libunity's window is static numeric data, not donor-process pointers: all
 * 4612 float words are finite, 4610 are in [-1,1], and the final words are
 * small integer descriptors.  The ELF has no relocation whose target lies in
 * this range, so restoring it after relocation cannot overwrite a rebased
 * address.  The decisive runtime proof is the Loading.Preload fault without
 * this overlay: Unity+0xe2e438 indexes this exact window as a float table and
 * the ciphertext supplies a wild signed index.
 */

static uint64_t fnv1a64(const void *data, size_t n)
{
    const unsigned char *p = data;
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    while (n--) {
        h ^= *p++;
        h *= UINT64_C(0x100000001b3);
    }
    return h;
}

static void read_exact_file(const char *path, void *dst, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        nx_die("open protected text %s: %s", path, strerror(errno));

    struct stat st;
    if (fstat(fd, &st) != 0)
        nx_die("stat protected text %s: %s", path, strerror(errno));
    if ((uint64_t)st.st_size != (uint64_t)size)
        nx_die("%s has %lld bytes; expected %zu for v1.09.3",
               path, (long long)st.st_size, size);

    size_t done = 0;
    while (done < size) {
        ssize_t got = read(fd, (unsigned char *)dst + done, size - done);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            nx_die("short read from protected text %s", path);
        done += (size_t)got;
    }
    close(fd);
}

int pf2_apply_text_overlays(void)
{
    char path[1280];
    for (size_t i = 0; i < sizeof overlays / sizeof *overlays; i++) {
        const text_overlay *o = &overlays[i];
        nx_mod *m = nx_find_mod(o->soname);
        if (!m)
            nx_die("cannot apply text: %s is not loaded", o->soname);
        if (o->vaddr > m->span || o->size > m->span - o->vaddr)
            nx_die("%s protected text range is outside its mapped image",
                   o->soname);

        unsigned char *buf = malloc(o->size);
        if (!buf)
            nx_die("out of memory reading %s protected text", o->soname);
        snprintf(path, sizeof path, "%s/lib/%s", pf2_gamedir, o->file);
        read_exact_file(path, buf, o->size);

        uint64_t hash = fnv1a64(buf, o->size);
        if (hash != o->fnv1a)
            nx_die("%s is not the v1.09.3 arm64 protected text "
                   "(FNV-1a %016llx, expected %016llx)",
                   path, (unsigned long long)hash,
                   (unsigned long long)o->fnv1a);

        void *dst = m->base + o->vaddr;
        memcpy(dst, buf, o->size);
        __builtin___clear_cache(dst, (unsigned char *)dst + o->size);
        free(buf);
        fprintf(stderr, "[pf2] restored %s %s at +%#lx (%zu bytes)\n",
                o->soname, o->file, (unsigned long)o->vaddr, o->size);
    }
    return 0;
}

/* PairIP encrypts one window of *writable* data per protected library as well,
 * the same 0xc7b0 budget it spends on .text.  In libil2cpp that window falls in
 * the IL2CPP metadata-usage table: an array of `(kind << 29) | (index << 1) | 1`
 * tokens that il2cpp_codegen_initialize_runtime_metadata resolves on first use.
 * We have no plaintext for these two windows, so a token read from them is
 * garbage and the resolver indexes a metadata table with a ~233-million entry
 * index.
 *
 * A token whose bit 0 is clear makes the resolver return immediately, so
 * zeroing the window converts "wild index" into "this usage stays NULL".  That
 * is not a fix -- it is the instrument that measures how many of the 6390 slots
 * the game actually needs.  It stays behind an environment variable and off in
 * the shipped binary.
 */
typedef struct {
    const char *soname;
    uintptr_t vaddr;
    size_t size;
} data_window;

static const data_window encrypted_data[] = {
    { "libil2cpp.so", 0x02f0e7a0, 0xc7b0 },
    { "libunity.so",  0x011cdbf8, 0x4810 },
};

int pf2_neutralise_encrypted_data(void)
{
    if (!getenv("PF2_ZERO_ENC_DATA"))
        return 0;

    for (size_t i = 0; i < sizeof encrypted_data / sizeof *encrypted_data; i++) {
        const data_window *w = &encrypted_data[i];
        nx_mod *m = nx_find_mod(w->soname);
        if (!m)
            continue;
        if (w->vaddr > m->span || w->size > m->span - w->vaddr) {
            nx_log("%s encrypted data window is outside its image", w->soname);
            continue;
        }
        memset(m->base + w->vaddr, 0, w->size);
        fprintf(stderr,
                "[pf2] zeroed %s encrypted data at +%#lx (%zu bytes, "
                "%zu slots) -- measurement only\n",
                w->soname, (unsigned long)w->vaddr, w->size, w->size / 8);
    }
    return 0;
}

static void *execute_program_stub(const char *name, void **args, long nargs)
{
    if (pf2_capture_mode)
        return pf2_capture_execute_program(name, args, nargs);
    (void)args;
    nx_log("ExecuteProgram(%s, %ld) bypassed by native bootstrap",
           name ? name : "?", nargs);
    return NULL;
}

void *pf2_protected_sym(const char *name)
{
    if (name && strcmp(name, "ExecuteProgram") == 0)
        return (void *)(uintptr_t)execute_program_stub;
    return NULL;
}

static void *impl_of(const char *name)
{
    void *fn = nx_resolve_import(name);
    if (!fn)
        fn = pf2_android_sym(name);
    if (!fn)
        fn = pf2_egl_sym(name);
    if (!fn)
        fn = nx_lookup(name);
    return fn;
}

static void patch_table(nx_mod *m, const pf2_plt_ent *tab, size_t n,
                        int *filled, int *already, int *missing)
{
    for (size_t i = 0; i < n; i++) {
        unsigned slot = tab[i].slot;
        const char *name = tab[i].name;

        /* This weak TLS init thunk is intentionally unresolved on Android too;
         * the captured GOT contains zero and its caller checks for zero. */
        if (strcmp(name, "_ZTH15gDeferredAction") == 0) {
            nx_patch_pltgot(m, slot, NULL);
            continue;
        }

        /* A normal relocation has already replaced the link-time PLT offset
         * with a host or mapped-module pointer. */
        if (nx_read_pltgot(m, slot) >= (uintptr_t)m->span) {
            (*already)++;
            continue;
        }

        void *fn = impl_of(name);
        if (!fn) {
            (*missing)++;
            nx_log("%s PLT %u: no native implementation for %s",
                   m->name, slot, name);
            continue;
        }
        nx_patch_pltgot(m, slot, fn);
        (*filled)++;
        if (pf2_trace_plt)
            nx_log("%s PLT %u <- %s (%p)", m->name, slot, name, fn);
    }
}

static int patch_module(const char *soname,
                        const pf2_plt_ent *original, size_t original_n,
                        const pf2_plt_ent *captured, size_t captured_n)
{
    nx_mod *m = nx_find_mod(soname);
    if (!m)
        return -1;

    int filled = 0, already = 0, missing = 0;
    patch_table(m, original, original_n, &filled, &already, &missing);
    patch_table(m, captured, captured_n, &filled, &already, &missing);
    fprintf(stderr,
            "[pf2] %s PLT complete: %d restored, %d already relocated, "
            "%d missing\n",
            soname, filled, already, missing);
    return missing;
}

int pf2_patch_protected_plt(void)
{
    int a = patch_module(
        "libunity.so",
        pf2_unity_plt, sizeof pf2_unity_plt / sizeof *pf2_unity_plt,
        pf2_unity_captured_plt,
        sizeof pf2_unity_captured_plt / sizeof *pf2_unity_captured_plt);
    int b = patch_module(
        "libil2cpp.so",
        pf2_il2cpp_plt, sizeof pf2_il2cpp_plt / sizeof *pf2_il2cpp_plt,
        pf2_il2cpp_captured_plt,
        sizeof pf2_il2cpp_captured_plt / sizeof *pf2_il2cpp_captured_plt);
    return (a < 0 ? 0 : a) + (b < 0 ? 0 : b);
}
