/* Dead Trigger 2.1.0 ARM64 recursive ELF loader. */
#define _GNU_SOURCE

#include "dt.h"
#include "egl_sdl.h"

#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define DT_MAX_MODULES 8
#define DT_MODULE_MAGIC 0x445432314d4f4455ULL
#define DT_SYNTHETIC_HANDLE ((void *)(uintptr_t)0x44543210UL)

struct dt_module {
    uint64_t magic;
    char name[64];
    char path[PATH_MAX];
    void *mapping;
    size_t mapping_size;
    so_module *state;
    int constructors_started;
    int constructors_done;
};

static char g_root[PATH_MAX] = ".";
static struct dt_module g_modules[DT_MAX_MODULES];
static int g_module_count;
static struct dt_module *g_current;
static DynLibFunction *g_imports;
static int g_import_count;
static char g_dlerror[256];
static struct dt_module *g_unity;
static struct dt_module *g_il2cpp;

static const char *base_name(const char *path) {
    const char *slash = path ? strrchr(path, '/') : NULL;
    return slash ? slash + 1 : path;
}

void dt_set_game_root(const char *root) {
    if (root && *root) {
        snprintf(g_root, sizeof g_root, "%s", root);
        size_t length = strlen(g_root);
        while (length > 1 && g_root[length - 1] == '/')
            g_root[--length] = '\0';
    }
}

const char *dt_game_root(void) {
    return g_root;
}

uintptr_t dt_unity_base(void) {
    return g_unity ? (uintptr_t)g_unity->mapping : 0;
}

uintptr_t dt_il2cpp_base(void) {
    return g_il2cpp ? (uintptr_t)g_il2cpp->mapping : 0;
}

/* Names expected by the two proven read-only support components. */
uintptr_t ter_unity_base(void) {
    return dt_unity_base();
}

uintptr_t ff5_il2cpp_base(void) {
    return dt_il2cpp_base();
}

void dt_set_unity_module(dt_module *module) {
    g_unity = module;
}

void dt_set_il2cpp_module(dt_module *module) {
    g_il2cpp = module;
}

void dt_loader_set_imports(DynLibFunction *imports, int count) {
    g_imports = imports;
    g_import_count = count;
}

static int elf_span(const char *path, size_t *out_span) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return -1;
    Elf64_Ehdr header;
    if (fread(&header, 1, sizeof header, file) != sizeof header ||
        memcmp(header.e_ident, ELFMAG, SELFMAG) ||
        header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_machine != EM_AARCH64 ||
        header.e_phentsize != sizeof(Elf64_Phdr) ||
        header.e_phnum == 0) {
        fclose(file);
        errno = ENOEXEC;
        return -1;
    }
    if (fseeko(file, (off_t)header.e_phoff, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }
    size_t maximum = 0;
    for (unsigned i = 0; i < header.e_phnum; ++i) {
        Elf64_Phdr program;
        if (fread(&program, 1, sizeof program, file) != sizeof program) {
            fclose(file);
            errno = ENOEXEC;
            return -1;
        }
        if (program.p_type == PT_LOAD) {
            size_t alignment = program.p_align ? (size_t)program.p_align : 1;
            size_t aligned_memory =
                ((size_t)program.p_memsz + alignment - 1) &
                ~(alignment - 1);
            if (program.p_vaddr + aligned_memory > maximum)
                maximum = (size_t)program.p_vaddr + aligned_memory;
        }
    }
    fclose(file);
    if (!maximum) {
        errno = ENOEXEC;
        return -1;
    }
    *out_span = (maximum + 0xffffu) & ~(size_t)0xffffu;
    return 0;
}

static void module_path(const char *name, char *path, size_t path_size) {
    if (name && strchr(name, '/')) {
        snprintf(path, path_size, "%s", name);
        if (access(path, R_OK) == 0)
            return;

        /*
         * Unity derives libil2cpp.so from Application.dataPath on this APK
         * and asks for <game-root>/libil2cpp.so even though NXExtract keeps
         * all owner Android ELFs in the canonical <game-root>/lib directory.
         * Resolve only the basename below our own root; do not require a
         * duplicate ELF at the package root and never follow the requested
         * directory into an arbitrary location.
         */
        const char *base = base_name(name);
        snprintf(path, path_size, "%s/lib/%s", g_root,
                 base ? base : "");
        return;
    }
    snprintf(path, path_size, "%s/lib/%s", g_root, name ? name : "");
    if (access(path, R_OK) != 0)
        snprintf(path, path_size, "%s/%s", g_root, name ? name : "");
}

dt_module *dt_module_find(const char *name) {
    const char *base = base_name(name);
    if (!base)
        return NULL;
    for (int i = 0; i < g_module_count; ++i)
        if (!strcmp(g_modules[i].name, base))
            return &g_modules[i];
    return NULL;
}

static int module_is_valid(const void *handle) {
    if (!handle)
        return 0;
    for (int i = 0; i < g_module_count; ++i)
        if (handle == &g_modules[i] && g_modules[i].magic == DT_MODULE_MAGIC)
            return 1;
    return 0;
}

dt_module *dt_module_load(const char *requested_name) {
    const char *name = base_name(requested_name);
    if (!name || !*name) {
        snprintf(g_dlerror, sizeof g_dlerror, "nome de modulo vazio");
        return NULL;
    }
    dt_module *existing = dt_module_find(name);
    if (existing)
        return existing;
    if (g_module_count >= DT_MAX_MODULES) {
        snprintf(g_dlerror, sizeof g_dlerror, "limite de modulos atingido");
        return NULL;
    }

    char path[PATH_MAX];
    module_path(requested_name, path, sizeof path);
    size_t span = 0;
    if (elf_span(path, &span) != 0) {
        snprintf(g_dlerror, sizeof g_dlerror, "%s: %s", path, strerror(errno));
        return NULL;
    }

    void *mapping = mmap(NULL, span, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        snprintf(g_dlerror, sizeof g_dlerror, "mmap %s: %s", name,
                 strerror(errno));
        return NULL;
    }

    struct dt_module *previous = g_current;
    struct dt_module *module = &g_modules[g_module_count];
    memset(module, 0, sizeof *module);
    module->magic = DT_MODULE_MAGIC;
    snprintf(module->name, sizeof module->name, "%s", name);
    snprintf(module->path, sizeof module->path, "%s", path);
    module->mapping = mapping;
    module->mapping_size = span;

    fprintf(stderr, "[loader] mapeando %s (%zu MiB max) em %p\n",
            name, (span + 1024 * 1024 - 1) / (1024 * 1024), mapping);
    if (so_load(path, mapping, span) != 0 ||
        so_relocate() != 0 ||
        so_resolve(g_imports, g_import_count, 1) != 0) {
        munmap(mapping, span);
        memset(module, 0, sizeof *module);
        if (previous) {
            so_use(previous->state);
            g_current = previous;
        }
        snprintf(g_dlerror, sizeof g_dlerror, "falha ELF ao carregar %s", name);
        return NULL;
    }

    so_record_phdr(name);
    module->state = so_save();
    g_module_count++;
    g_current = module;

    if (!strcmp(name, "libunity.so"))
        g_unity = module;
    else if (!strcmp(name, "libil2cpp.so"))
        g_il2cpp = module;

    /*
     * Constructors are part of loading, exactly as Android's linker does.
     * Save the module first because a constructor can recursively dlopen
     * libil2cpp; the nested load then restores this active loader state.
     */
    so_finalize();
    module->constructors_started = 1;
    so_execute_init_array();
    module->constructors_done = 1;
    fprintf(stderr, "[loader] %s pronto; init_array concluido\n", name);

    if (previous) {
        so_use(previous->state);
        g_current = previous;
    } else {
        so_use(module->state);
        g_current = module;
    }
    return module;
}

void *dt_module_symbol(dt_module *module, const char *name) {
    if (!module || !name)
        return NULL;
    struct dt_module *previous = g_current;
    so_use(module->state);
    g_current = module;
    void *result = (void *)so_find_addr_safe(name);
    if (previous) {
        so_use(previous->state);
        g_current = previous;
    }
    return result;
}

void *dt_module_base(dt_module *module) {
    return module ? module->mapping : NULL;
}

size_t dt_module_size(dt_module *module) {
    return module ? module->mapping_size : 0;
}

const char *dt_module_name(dt_module *module) {
    return module ? module->name : "";
}

static const char *linux_library_name(const char *name) {
    if (!name)
        return NULL;
    if (!strcmp(name, "libc.so")) return "libc.so.6";
    if (!strcmp(name, "libm.so")) return "libm.so.6";
    if (!strcmp(name, "libdl.so")) return "libdl.so.2";
    if (!strcmp(name, "libz.so")) return "libz.so.1";
    if (!strcmp(name, "libEGL.so")) return "libEGL.so.1";
    if (!strcmp(name, "libGLESv2.so")) return "libGLESv2.so.2";
    return name;
}

void *dt_dlopen(const char *filename, int flags) {
    const char *name = base_name(filename);
    if (getenv("DT_DL_TRACE"))
        fprintf(stderr, "[dl] open '%s' flags=0x%x\n",
                filename ? filename : "(null)", flags);
    if (!name) {
        void *handle = dlopen(NULL, flags);
        if (!handle)
            snprintf(g_dlerror, sizeof g_dlerror, "%s", dlerror());
        return handle;
    }
    if (!strcmp(name, "libmain.so") ||
        !strcmp(name, "libunity.so") ||
        !strcmp(name, "libil2cpp.so"))
        return dt_module_load(filename);

    if (!strcmp(name, "libandroid.so") ||
        !strcmp(name, "liblog.so") ||
        !strcmp(name, "libOpenSLES.so"))
        return DT_SYNTHETIC_HANDLE;

    void *handle = dlopen(linux_library_name(name), flags);
    if (!handle) {
        const char *error = dlerror();
        snprintf(g_dlerror, sizeof g_dlerror, "%s",
                 error ? error : "dlopen desconhecido");
    }
    return handle;
}

static void *import_symbol(const char *name) {
    for (int i = 0; i < g_import_count; ++i)
        if (!strcmp(g_imports[i].symbol, name))
            return (void *)g_imports[i].func;
    return NULL;
}

void *dt_dlsym(void *handle, const char *name) {
    if (!name)
        return NULL;
    if (dt_sdl_video_active()) {
        void *video_symbol = NULL;
        if (!strncmp(name, "egl", 3))
            video_symbol = dt_sdl_egl_proc(name);
        else if (!strncmp(name, "gl", 2))
            video_symbol = dt_sdl_gl_proc(name);
        if (video_symbol)
            return video_symbol;
    }
    if (module_is_valid(handle)) {
        void *result = dt_module_symbol((dt_module *)handle, name);
        if (getenv("DT_DL_TRACE"))
            fprintf(stderr, "[dl] symbol module %s -> %p\n", name, result);
        return result;
    }
    if (handle == DT_SYNTHETIC_HANDLE) {
        void *result = import_symbol(name);
        if (getenv("DT_DL_TRACE") || !result)
            fprintf(stderr, "[dl] symbol Android %s -> %p\n", name, result);
        return result;
    }
    if (!handle || handle == RTLD_DEFAULT) {
        for (int i = 0; i < g_module_count; ++i) {
            void *symbol = dt_module_symbol(&g_modules[i], name);
            if (symbol)
                return symbol;
        }
        void *special = import_symbol(name);
        if (special)
            return special;
        return dlsym(RTLD_DEFAULT, name);
    }
    return dlsym(handle, name);
}

int dt_dlclose(void *handle) {
    if (!handle || module_is_valid(handle) || handle == DT_SYNTHETIC_HANDLE)
        return 0;
    return dlclose(handle);
}

char *dt_dlerror(void) {
    if (*g_dlerror) {
        static char result[256];
        snprintf(result, sizeof result, "%s", g_dlerror);
        g_dlerror[0] = '\0';
        return result;
    }
    return dlerror();
}

int dt_dladdr(const void *address, void *opaque_info) {
    Dl_info *info = (Dl_info *)opaque_info;
    uintptr_t pointer = (uintptr_t)address;
    for (int i = 0; i < g_module_count; ++i) {
        uintptr_t base = (uintptr_t)g_modules[i].mapping;
        if (pointer >= base && pointer < base + g_modules[i].mapping_size) {
            if (info) {
                memset(info, 0, sizeof *info);
                info->dli_fname = g_modules[i].path;
                info->dli_fbase = g_modules[i].mapping;
            }
            return 1;
        }
    }
    return dladdr(address, info);
}

int dl_iterate_phdr(int (*callback)(struct dl_phdr_info *, size_t, void *),
                    void *data) {
    static int (*real_iterate)(int (*)(struct dl_phdr_info *, size_t, void *),
                               void *);
    if (!real_iterate)
        real_iterate = (void *)dlsym(RTLD_NEXT, "dl_iterate_phdr");
    int result = real_iterate ? real_iterate(callback, data) : 0;
    if (result)
        return result;
    for (int i = 0; i < g_so_nmods; ++i) {
        struct dl_phdr_info info;
        memset(&info, 0, sizeof info);
        info.dlpi_addr = (ElfW(Addr))g_so_mods[i].base;
        info.dlpi_name = g_so_mods[i].name;
        info.dlpi_phdr = (const ElfW(Phdr) *)g_so_mods[i].ph;
        info.dlpi_phnum = (ElfW(Half))g_so_mods[i].phnum;
        result = callback(&info, sizeof info, data);
        if (result)
            return result;
    }
    return 0;
}

int dt_loader_init(void) {
    const char *libraries[] = {
        "libz.so.1", "libEGL.so.1", "libGLESv2.so.2", "libm.so.6",
        "libdl.so.2", NULL
    };
    for (int i = 0; libraries[i]; ++i) {
        if (!dlopen(libraries[i], RTLD_NOW | RTLD_GLOBAL))
            fprintf(stderr, "[loader] aviso: %s: %s\n", libraries[i],
                    dlerror());
    }
    return 0;
}
