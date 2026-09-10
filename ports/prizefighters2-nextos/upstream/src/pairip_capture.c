/*
 * pairip_capture.c -- one-shot recovery helper for Prizefighters 2 v1.09.3.
 *
 * This is not an Android bootstrap and is never entered by the shipped game
 * path.  PF2_VM_CAPTURE=1 loads the original arm64 PairIP interpreter inside
 * the same native NextOS process, follows the application's startup order, and
 * records only the writable bytes changed by each library's real DT_INIT.
 * Those bytes can then be version-pinned like the already recovered .text
 * overlays, leaving normal gameplay independent of PairIP.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ucontext.h>
#include <unistd.h>

#include "nx_elf.h"
#include "pf2.h"

#define PROG_STARTUP "3H0fStvCiQxkzzNR"

/* This instruction begins PairIP's self-hash opcode in this exact
 * libpairipcore build.  The startup program deliberately supplies a negative
 * signed count immediately after its Play Integrity telemetry path.  Android
 * terminates there; for extraction we turn that one count into zero and let
 * the original interpreter continue. */
#define SELFHASH_OFF  0x53ac0u
#define SELFHASH_INSN 0xf94002a1u /* ldr x1, [x21] */
#define BRK_INSN      0xd4200000u
#define VM_PC_OFF     12u

static uint32_t *selfhash_site;
static volatile sig_atomic_t bypass_negative_selfhash;
static volatile sig_atomic_t bypass_hits;
static volatile int last_count;
static volatile unsigned last_vm_pc;

static uint32_t *patch_instruction(nx_mod *m, unsigned off, uint32_t expected)
{
    if (!m || off > m->span - sizeof(uint32_t))
        return NULL;
    uint32_t *site = (uint32_t *)(m->base + off);
    if (*site != expected) {
        nx_log("capture: pairip+%#x is %#x, expected %#x",
               off, *site, expected);
        return NULL;
    }
    long pagesz = sysconf(_SC_PAGESIZE);
    uintptr_t page = (uintptr_t)site & ~(uintptr_t)(pagesz - 1);
    if (mprotect((void *)page, (size_t)pagesz,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        nx_log("capture: mprotect pairip trap: %s", strerror(errno));
        return NULL;
    }
    *site = BRK_INSN;
    __builtin___clear_cache((char *)site, (char *)site + sizeof *site);
    return site;
}

static void on_capture_trap(int sig, siginfo_t *si, void *opaque)
{
    (void)sig;
    (void)si;
    ucontext_t *u = opaque;
    if (!selfhash_site ||
        u->uc_mcontext.pc != (uintptr_t)selfhash_site) {
        _exit(121);
    }

    uintptr_t desc = (uintptr_t)u->uc_mcontext.regs[21];
    if (desc) {
        uint8_t *program = *(uint8_t **)desc;
        uint32_t len = *(uint32_t *)(desc + 8);
        uint32_t pc = *(uint32_t *)(desc + VM_PC_OFF);
        if (program && pc <= len && len - pc > 0x1d) {
            uint32_t at = pc + 0x1c;
            int16_t count = (int16_t)((uint16_t)program[at] |
                                      ((uint16_t)program[at + 1] << 8));
            last_count = count;
            last_vm_pc = pc;
            if (bypass_negative_selfhash && count < 0) {
                program[at] = 0;
                program[at + 1] = 0;
                bypass_hits++;
            }
        }
        /* Emulate the ldr instruction displaced by BRK. */
        u->uc_mcontext.regs[1] = *(uint64_t *)desc;
    }
    u->uc_mcontext.pc += sizeof(uint32_t);
}

static int install_capture_trap(void)
{
    nx_mod *pairip = nx_find_mod("libpairipcore.so");
    selfhash_site = patch_instruction(pairip, SELFHASH_OFF, SELFHASH_INSN);
    if (!selfhash_site)
        return -1;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_capture_trap;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGTRAP, &sa, NULL) != 0)
        return -1;
    return 0;
}

static void *read_program(const char *name, int *len_out)
{
    char path[1280];
    snprintf(path, sizeof path, "%s/%s", pf2_datadir, name);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        nx_log("capture: open %s: %s", path, strerror(errno));
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 8 ||
        st.st_size > 16 * 1024 * 1024) {
        close(fd);
        return NULL;
    }
    uint8_t *buf = malloc((size_t)st.st_size);
    if (!buf) {
        close(fd);
        return NULL;
    }
    size_t done = 0;
    while (done < (size_t)st.st_size) {
        ssize_t got = read(fd, buf + done, (size_t)st.st_size - done);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0) {
            free(buf);
            close(fd);
            return NULL;
        }
        done += (size_t)got;
    }
    close(fd);
    if (!(buf[0] == 0 && buf[1] == 'I' &&
          buf[2] == 'A' && buf[3] == 'P')) {
        free(buf);
        return NULL;
    }
    *len_out = (int)done;
    nx_log("capture: IAP %s, %zu bytes, format v%u",
           name, done, buf[4]);
    return buf;
}

/* JNI helpers deliberately stay private to jni.c's implementation.  Their C
 * ABI is plain pointers, which is all this bridge needs. */
extern void pf2_jni_bind(const char *, const char *, const char *, void *);
extern void *pf2_jret_bytes(const void *, int);
extern void *pf2_jret_class(const char *);
extern const char *pf2_jarg_str(void *);
extern void *pf2_jarg_obj(void *);

static long vmrunner_getcontext(void *ctx)
{
    (void)ctx;
    return (long)(uintptr_t)pf2_jret_obj("android/content/Context");
}

static long invoke_program(const char *name, void *args)
{
    int len = 0;
    void *program = read_program(name, &len);
    if (!program)
        return 0;

    typedef void *(*execute_vm)(void *, void *, void *, void *);
    execute_vm execute = (execute_vm)pf2_jni_native(
        "com/pairip/VMRunner", "executeVM");
    if (!execute) {
        free(program);
        nx_log("capture: PairIP did not register executeVM");
        return 0;
    }

    void *bytes = pf2_jret_bytes(program, len);
    free(program);
    void *cls = pf2_jret_class("com/pairip/VMRunner");
    fprintf(stderr, "[pf2] capture: executeVM(%s) ...\n", name);
    void *ret = execute(pf2_jni_env(), cls, bytes, args);
    fprintf(stderr, "[pf2] capture: executeVM(%s) -> %p\n", name, ret);
    return (long)(uintptr_t)ret;
}

static long vmrunner_invoke(void *ctx)
{
    const char *name = pf2_jarg_str(ctx);
    void *args = pf2_jarg_obj(ctx);
    return name ? invoke_program(name, args) : 0;
}

void *pf2_capture_execute_program(const char *name, void **args, long nargs)
{
    static void *(*execute)(const char *, void **, long);
    if (!execute) {
        nx_mod *pairip = nx_find_mod("libpairipcore.so");
        execute = pairip ? (void *(*)(const char *, void **, long))
                    nx_lookup_in(pairip, "ExecuteProgram") : NULL;
    }
    if (!execute) {
        nx_log("capture: real ExecuteProgram is unavailable");
        return NULL;
    }
    fprintf(stderr, "[pf2] capture: ExecuteProgram(%s, %ld)\n",
            name ? name : "?", nargs);
    return execute(name, args, nargs);
}

typedef struct {
    uint64_t vaddr;
    size_t size;
    uint8_t *before;
} snap_range;

typedef struct {
    nx_mod *mod;
    snap_range ranges[8];
    size_t count;
} module_snapshot;

static int add_snapshot(module_snapshot *s, uint64_t vaddr, size_t size)
{
    if (!size || s->count == sizeof s->ranges / sizeof s->ranges[0])
        return -1;
    if (vaddr > s->mod->span || size > s->mod->span - vaddr)
        return -1;
    snap_range *r = &s->ranges[s->count++];
    r->vaddr = vaddr;
    r->size = size;
    r->before = malloc(size);
    if (!r->before)
        return -1;
    memcpy(r->before, s->mod->base + vaddr, size);
    return 0;
}

static int take_snapshot(module_snapshot *s, nx_mod *m)
{
    memset(s, 0, sizeof *s);
    s->mod = m;
    for (size_t i = 0; i < m->phnum; i++) {
        Elf64_Phdr *p = &m->phdr[i];
        if (p->p_type == PT_LOAD && (p->p_flags & PF_W))
            if (add_snapshot(s, p->p_vaddr, p->p_memsz) != 0)
                return -1;
    }

    /* PairIP also replaces exactly 0xc800 bytes at each protected entry point.
     * Keep them in the diagnostic delta even though production already has
     * independently captured and hashed copies. */
    if (strcmp(m->name, "libil2cpp.so") == 0)
        return add_snapshot(s, 0x118bf7c, 0xc800);
    if (strcmp(m->name, "libunity.so") == 0)
        return add_snapshot(s, 0x397710, 0xc800);
    return 0;
}

struct delta_header {
    char magic[8];
    uint32_t version;
    uint32_t records;
    uint64_t module_base;
};

struct delta_record {
    uint64_t vaddr;
    uint64_t size;
};

static int write_all(int fd, const void *data, size_t size)
{
    const uint8_t *p = data;
    while (size) {
        ssize_t done = write(fd, p, size);
        if (done < 0 && errno == EINTR)
            continue;
        if (done <= 0)
            return -1;
        p += done;
        size -= (size_t)done;
    }
    return 0;
}

typedef struct {
    const char *soname;
    const char *file;
    uint64_t vaddr;
    size_t size;
} capture_overlay;

/* These are the exact plaintext windows consumed by protected.c in normal
 * gameplay.  Capture mode is BYO-data: write them beside the diagnostic delta
 * so an owner of the exact v1.09.3 arm64 split can reproduce the production
 * input without manually slicing process memory. */
static const capture_overlay capture_overlays[] = {
    { "libil2cpp.so", "libil2cpp.text", 0x0118bf7c, 0x0000c800 },
    { "libil2cpp.so", "libil2cpp.data", 0x02f0e7a0, 0x0000c7b0 },
    { "libunity.so",  "libunity.text",  0x00397710, 0x0000c800 },
    { "libunity.so",  "libunity.data",  0x011cdbf8, 0x00004810 },
};

static uint64_t capture_fnv1a64(const void *data, size_t size)
{
    const uint8_t *p = data;
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    while (size--) {
        hash ^= *p++;
        hash *= UINT64_C(0x100000001b3);
    }
    return hash;
}

static int write_captured_overlays(nx_mod *m)
{
    char dir[1200], path[1400];
    const char *override = getenv("PF2_CAPTURE_DIR");
    snprintf(dir, sizeof dir, "%s",
             override && *override ? override : pf2_home);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        return -1;

    for (size_t i = 0;
         i < sizeof capture_overlays / sizeof *capture_overlays; i++) {
        const capture_overlay *overlay = &capture_overlays[i];
        if (strcmp(overlay->soname, m->name) != 0)
            continue;
        if (overlay->vaddr > m->span ||
            overlay->size > m->span - overlay->vaddr)
            return -1;

        snprintf(path, sizeof path, "%s/%s", dir, overlay->file);
        int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
        if (fd < 0)
            return -1;
        const void *data = m->base + overlay->vaddr;
        int rc = write_all(fd, data, overlay->size);
        if (rc == 0)
            rc = fsync(fd);
        if (close(fd) != 0 && rc == 0)
            rc = -1;
        if (rc != 0)
            return -1;

        fprintf(stderr,
                "[pf2] capture: wrote %s (%zu bytes, FNV-1a %016llx)\n",
                path, overlay->size,
                (unsigned long long)capture_fnv1a64(data, overlay->size));
    }
    return 0;
}

static int write_delta(module_snapshot *s)
{
    char dir[1200], path[1400];
    const char *override = getenv("PF2_CAPTURE_DIR");
    snprintf(dir, sizeof dir, "%s",
             override && *override ? override : pf2_home);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        return -1;
    snprintf(path, sizeof path, "%s/%s.pf2delta", dir, s->mod->name);

    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    struct delta_header h = {
        .magic = { 'P', 'F', '2', 'D', 'L', 'T', '1', 0 },
        .version = 1,
        .module_base = (uint64_t)(uintptr_t)s->mod->base,
    };
    if (write_all(fd, &h, sizeof h) != 0) {
        close(fd);
        return -1;
    }

    uint64_t changed = 0;
    for (size_t k = 0; k < s->count; k++) {
        snap_range *r = &s->ranges[k];
        const uint8_t *after = s->mod->base + r->vaddr;
        size_t i = 0;
        while (i < r->size) {
            while (i < r->size && r->before[i] == after[i])
                i++;
            if (i == r->size)
                break;
            size_t start = i, last = i;
            for (i++; i < r->size; i++) {
                if (r->before[i] != after[i])
                    last = i;
                else if (i - last > 32)
                    break;
            }
            size_t size = last + 1 - start;
            struct delta_record rec = {
                .vaddr = r->vaddr + start,
                .size = size,
            };
            if (write_all(fd, &rec, sizeof rec) != 0 ||
                write_all(fd, r->before + start, size) != 0 ||
                write_all(fd, after + start, size) != 0) {
                close(fd);
                return -1;
            }
            h.records++;
            changed += size;
        }
    }
    if (lseek(fd, 0, SEEK_SET) < 0 || write_all(fd, &h, sizeof h) != 0 ||
        fsync(fd) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    fprintf(stderr,
            "[pf2] capture: %s wrote %u ranges, %llu changed bytes to %s\n",
            s->mod->name, h.records, (unsigned long long)changed, path);
    return 0;
}

static void free_snapshot(module_snapshot *s)
{
    for (size_t i = 0; i < s->count; i++)
        free(s->ranges[i].before);
    memset(s, 0, sizeof *s);
}

static int capture_dt_init(nx_mod *m)
{
    module_snapshot snap;
    if (!m || take_snapshot(&snap, m) != 0)
        return -1;
    if (!m->init_func) {
        free_snapshot(&snap);
        return -1;
    }

    fprintf(stderr, "[pf2] capture: real DT_INIT(%s) ...\n", m->name);
    void (*init)(void) = m->init_func;
    m->init_func = NULL;
    init();
    fprintf(stderr, "[pf2] capture: real DT_INIT(%s) returned\n", m->name);
    int rc = write_delta(&snap);
    if (rc == 0)
        rc = write_captured_overlays(m);
    free_snapshot(&snap);
    return rc;
}

int pf2_pairip_capture(void)
{
    nx_mod *pairip = nx_find_mod("libpairipcore.so");
    nx_mod *main_mod = nx_find_mod("libmain.so");
    nx_mod *il2cpp = nx_find_mod("libil2cpp.so");
    nx_mod *unity = nx_find_mod("libunity.so");
    if (!pairip || !main_mod || !il2cpp || !unity)
        return -1;

    /* PairIP is loaded by VMRunner.<clinit>, then Application.<clinit> invokes
     * the startup program before Unity's NativeLoader touches the game DSOs. */
    nx_run_init(pairip);
    if (install_capture_trap() != 0)
        return -1;
    pf2_jni_bind("com/pairip/VMRunner", "getContext",
                 "()Landroid/content/Context;", (void *)vmrunner_getcontext);
    pf2_jni_bind("com/pairip/VMRunner", "invoke",
                 "(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/Object;",
                 (void *)vmrunner_invoke);

    typedef int (*onload)(void *, void *);
    onload jni_onload = (onload)nx_lookup_in(pairip, "JNI_OnLoad");
    if (!jni_onload)
        return -1;
    int version = jni_onload(pf2_jni_vm(), NULL);
    fprintf(stderr, "[pf2] capture: JNI_OnLoad(libpairipcore) -> %#x\n",
            version);
    if (!pf2_jni_native("com/pairip/VMRunner", "executeVM"))
        return -1;

    bypass_negative_selfhash = 1;
    invoke_program(PROG_STARTUP, NULL);
    bypass_negative_selfhash = 0;
    fprintf(stderr,
            "[pf2] capture: startup returned; self-hash pc=%u count=%d, "
            "bypassed=%d\n",
            last_vm_pc, last_count, bypass_hits);
    if (!bypass_hits)
        return -1;

    /* NativeLoader order: libmain, libil2cpp, libunity.  Observe each protected
     * DT_INIT immediately after it runs, then continue with that object's real
     * init array before loading the next one. */
    nx_run_init(main_mod);
    if (capture_dt_init(il2cpp) != 0)
        return -1;
    nx_run_init(il2cpp);
    if (capture_dt_init(unity) != 0)
        return -1;
    nx_run_init(unity);
    return 0;
}
