/* Strict nxloader adapter for the Gunbrick Unity guest. */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nx_elf.h"

#define GB_MAX_GUEST_FILE (512u * 1024u * 1024u)
#define GB_MAX_GUEST_IMAGE (1024u * 1024u * 1024u)
#define GB_TRAMPOLINE_POOL (64u * 1024u)
#define GB_HOST_PRIORITY 1000
#define GB_FIRST_GUEST_PRIORITY 100

int nx_verbose;

static nx_mod *modules_head;
static nx_mod *modules_tail;
static size_t module_count;
static const nx_import *imports;
static size_t imports_n;
static nxloader_registry *registry;

void nx_log(const char *fmt, ...)
{
    va_list ap;

    if (!nx_verbose)
        return;
    va_start(ap, fmt);
    fputs("[gunbrick] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fflush(stderr);
}

void nx_die(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fputs("[gunbrick] FATAL: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fflush(stderr);
    _exit(1);
}

static void loader_log(void *userdata, nxloader_log_level level,
                       const char *message)
{
    (void)userdata;
    if (level <= NXLOADER_LOG_WARNING || nx_verbose)
        fprintf(stderr, "[gunbrick/nxloader] %s\n",
                message ? message : "(no diagnostic)");
}

static int valid_name(const char *name)
{
    return name && name[0] && strnlen(name, 4096) < 4096;
}

int nx_set_imports(const nx_import *tab, size_t n)
{
    nxloader_symbol *symbols;
    nxloader_provider provider;
    nxloader_registry *candidate = NULL;
    nxloader_registry_report report;
    nxloader_result result;
    size_t i;

    if (registry || !tab || !n || n > SIZE_MAX / sizeof(*symbols))
        return -1;
    for (i = 0; i < n; ++i) {
        if (!valid_name(tab[i].name) || !tab[i].addr)
            return -1;
        if (i && strcmp(tab[i - 1].name, tab[i].name) > 0)
            return -1;
    }

    symbols = calloc(n, sizeof(*symbols));
    if (!symbols)
        return -1;
    for (i = 0; i < n; ++i) {
        symbols[i].name = tab[i].name;
        symbols[i].address = (uintptr_t)tab[i].addr;
    }
    memset(&provider, 0, sizeof(provider));
    provider.struct_size = sizeof(provider);
    provider.name = "gunbrick-host-imports-v1";
    provider.symbols = symbols;
    provider.symbol_count = n;
    provider.priority = GB_HOST_PRIORITY;
    memset(&report, 0, sizeof(report));
    report.struct_size = sizeof(report);

    result = nxloader_registry_create(&candidate);
    if (result == NXLOADER_OK)
        result = nxloader_registry_add_provider(candidate, &provider, &report);
    free(symbols);
    if (result != NXLOADER_OK) {
        fprintf(stderr,
                "[gunbrick/nxloader] host import registry rejected: %s\n",
                nxloader_result_string(result));
        nxloader_registry_destroy(candidate);
        return -1;
    }

    registry = candidate;
    imports = tab;
    imports_n = n;
    nx_log("host provider: %zu symbols (%zu equivalent duplicates)",
           report.added, report.equivalent);
    return 0;
}

void *nx_resolve_import(const char *symbol)
{
    size_t lo = 0;
    size_t hi = imports_n;

    if (!valid_name(symbol))
        return NULL;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int order = strcmp(imports[mid].name, symbol);
        if (!order)
            return imports[mid].addr;
        if (order < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

static int read_exact_at(int fd, void *buffer, size_t size, off_t offset)
{
    unsigned char *out = buffer;
    size_t done = 0;

    while (done < size) {
        ssize_t got = pread(fd, out + done, size - done,
                            offset + (off_t)done);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return -1;
        done += (size_t)got;
    }
    return 0;
}

static int read_program_headers(const char *path, Elf64_Phdr **out_headers,
                                size_t *out_count)
{
    struct stat status;
    Elf64_Ehdr header;
    Elf64_Phdr *headers = NULL;
    uint64_t table_size;
    uint64_t table_end;
    int fd = -1;

    *out_headers = NULL;
    *out_count = 0;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &status) != 0 || status.st_size <= 0)
        goto fail;
    if (read_exact_at(fd, &header, sizeof(header), 0) != 0 ||
        memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
        header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_ident[EI_DATA] != ELFDATA2LSB ||
        header.e_machine != EM_AARCH64 || header.e_phnum == 0 ||
        header.e_phentsize != sizeof(Elf64_Phdr))
        goto fail;
    table_size = (uint64_t)header.e_phnum * sizeof(Elf64_Phdr);
    if (header.e_phoff > UINT64_MAX - table_size)
        goto fail;
    table_end = header.e_phoff + table_size;
    if (table_end > (uint64_t)status.st_size || table_size > SIZE_MAX)
        goto fail;
    headers = malloc((size_t)table_size);
    if (!headers || read_exact_at(fd, headers, (size_t)table_size,
                                  (off_t)header.e_phoff) != 0)
        goto fail;
    close(fd);
    *out_headers = headers;
    *out_count = header.e_phnum;
    return 0;

fail:
    if (fd >= 0)
        close(fd);
    free(headers);
    return -1;
}

nx_mod *nx_find_mod(const char *soname)
{
    nx_mod *module;

    if (!soname)
        return NULL;
    for (module = modules_head; module; module = module->next)
        if (strcmp(module->name, soname) == 0)
            return module;
    return NULL;
}

nx_mod *nx_load(const char *path, const char *soname)
{
    nxloader_config config;
    nxloader_module_info info;
    nxloader_module *guest = NULL;
    nxloader_result result;
    nx_mod *module = NULL;
    Elf64_Phdr *headers = NULL;
    size_t header_count = 0;
    int written;

    if (!registry || !path || !*path || !valid_name(soname) ||
        nx_find_mod(soname))
        return NULL;
    if (read_program_headers(path, &headers, &header_count) != 0) {
        fprintf(stderr, "[gunbrick/nxloader] invalid AArch64 ELF: %s\n", path);
        return NULL;
    }

    module = calloc(1, sizeof(*module));
    if (!module)
        goto fail;
    written = snprintf(module->name, sizeof(module->name), "%s", soname);
    if (written < 0 || (size_t)written >= sizeof(module->name))
        goto fail;
    written = snprintf(module->path, sizeof(module->path), "%s", path);
    if (written < 0 || (size_t)written >= sizeof(module->path))
        goto fail;

    nxloader_config_init(&config);
    config.expected_arch = NXLOADER_ARCH_AARCH64;
    config.flags |= NXLOADER_CONFIG_FILE_BACKED_TEXT;
    config.max_file_size = GB_MAX_GUEST_FILE;
    config.max_image_size = GB_MAX_GUEST_IMAGE;
    config.trampoline_pool_size = GB_TRAMPOLINE_POOL;
    config.log = loader_log;
    result = nxloader_module_create(&config, &guest);
    if (result == NXLOADER_OK)
        result = nxloader_module_load_file(guest, path);
    if (result != NXLOADER_OK) {
        fprintf(stderr, "[gunbrick/nxloader] load %s failed: %s\n", soname,
                nxloader_result_string(result));
        goto fail;
    }
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    result = nxloader_module_get_info(guest, &info);
    if (result != NXLOADER_OK || info.arch != NXLOADER_ARCH_AARCH64 ||
        info.minimum_vma != 0 || !info.mapping_base || !info.mapping_size) {
        fprintf(stderr,
                "[gunbrick/nxloader] %s violates pinned guest image contract\n",
                soname);
        goto fail;
    }

    module->module = guest;
    module->base = info.mapping_base;
    module->span = info.mapping_size;
    module->phdr = headers;
    module->phnum = header_count;
    module->load_index = module_count++;
    if (modules_tail)
        modules_tail->next = module;
    else
        modules_head = module;
    modules_tail = module;
    nx_log("loaded %-22s base=%p span=%zu", soname,
           (void *)module->base, module->span);
    return module;

fail:
    nxloader_module_destroy(guest);
    free(headers);
    free(module);
    return NULL;
}

int nx_relocate(nx_mod *module)
{
    nxloader_registry_report report;
    nxloader_result result;
    int priority;

    if (!module || !module->module)
        return -1;
    if (module->relocated)
        return 0;
    result = nxloader_module_relocate(module->module);
    if (result != NXLOADER_OK)
        goto fail;
    if (module->load_index > (size_t)(GB_FIRST_GUEST_PRIORITY - 1))
        goto fail;
    priority = GB_FIRST_GUEST_PRIORITY - (int)module->load_index;
    memset(&report, 0, sizeof(report));
    report.struct_size = sizeof(report);
    result = nxloader_registry_add_module(registry, module->module,
                                          module->name, priority, &report);
    if (result != NXLOADER_OK)
        goto fail;
    module->relocated = 1;
    nx_log("relocated/provider %-22s added=%zu priority=%d", module->name,
           report.added, priority);
    return 0;

fail:
    fprintf(stderr, "[gunbrick/nxloader] relocate/provider %s failed: %s\n",
            module->name, nxloader_result_string(result));
    return -1;
}

int nx_resolve(nx_mod *module)
{
    nxloader_resolution_report report;
    nxloader_result result;

    if (!module || !module->module || !module->relocated)
        return -1;
    if (module->resolved)
        return 0;
    memset(&report, 0, sizeof(report));
    report.struct_size = sizeof(report);
    result = nxloader_module_resolve(module->module, registry, 0, &report);
    if (result != NXLOADER_OK) {
        fprintf(stderr,
                "[gunbrick/nxloader] resolve %s failed: %s; first=%s strong=%zu\n",
                module->name, nxloader_result_string(result),
                report.first_unresolved ? report.first_unresolved : "(none)",
                report.unresolved_strong);
        return -1;
    }
    module->resolved = 1;
    nx_log("resolved %-22s imports=%zu weak-zero=%zu", module->name,
           report.imports_resolved, report.weak_imports_zeroed);
    return 0;
}

int nx_run_init(nx_mod *module)
{
    nxloader_state state;
    nxloader_result result;

    if (!module || !module->module || !module->resolved)
        return -1;
    if (module->inited)
        return 0;
    state = nxloader_module_get_state(module->module);
    if (state == NXLOADER_STATE_RESOLVED) {
        result = nxloader_module_finalize(module->module);
        if (result != NXLOADER_OK)
            goto fail;
        state = NXLOADER_STATE_FINALIZED;
    }
    if (state != NXLOADER_STATE_FINALIZED)
        return -1;
    result = nxloader_module_call_initializers(module->module);
    if (result != NXLOADER_OK)
        goto fail;
    module->inited = 1;
    nx_log("%s: W^X finalized and initializers complete", module->name);
    return 0;

fail:
    fprintf(stderr, "[gunbrick/nxloader] initialize %s failed: %s\n",
            module->name, nxloader_result_string(result));
    return -1;
}

int nx_call_jni_onload(nx_mod *module, void *java_vm,
                       int32_t *returned_version)
{
    static const int32_t accepted_versions[] = { 0x00010006 };
    nxloader_jni_onload_options options;
    nxloader_result result;
    int32_t version = 0;

    if (!module || !module->module || !java_vm || !returned_version ||
        !module->inited)
        return -1;
    if (module->ready) {
        *returned_version = module->jni_version;
        return 0;
    }
    memset(&options, 0, sizeof(options));
    options.struct_size = sizeof(options);
    options.java_vm = java_vm;
    options.accepted_versions = accepted_versions;
    options.accepted_version_count =
        sizeof(accepted_versions) / sizeof(accepted_versions[0]);
    result = nxloader_module_call_jni_onload(module->module, &options,
                                             &version);
    if (result != NXLOADER_OK) {
        fprintf(stderr, "[gunbrick/nxloader] JNI_OnLoad %s failed: %s\n",
                module->name, nxloader_result_string(result));
        return -1;
    }
    module->jni_version = version;
    module->ready = 1;
    *returned_version = version;
    nx_log("JNI_OnLoad(%s) -> %#x (allowlisted)", module->name,
           (unsigned)version);
    return 0;
}

void *nx_lookup_in(nx_mod *module, const char *symbol)
{
    uintptr_t address = 0;

    if (!module || !module->module || !valid_name(symbol) ||
        nxloader_module_find_export(module->module, symbol, &address) !=
            NXLOADER_OK)
        return NULL;
    return (void *)address;
}

void *nx_lookup(const char *symbol)
{
    nx_mod *module;
    void *address;

    for (module = modules_head; module; module = module->next) {
        address = nx_lookup_in(module, symbol);
        if (address)
            return address;
    }
    return NULL;
}
