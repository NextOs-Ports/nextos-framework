#define _GNU_SOURCE
#include "quality.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nxcompat_settings.h"
#include "nx_elf.h"

static nxgl_quality_state quality_state;
static int quality_initialized;
static int quality_ready;
static int runtime_applied;
static unsigned long runtime_applied_frame;
static unsigned runtime_attempts;
static unsigned long runtime_next_attempt;

typedef void *(*sf_il2cpp_domain_get_fn)(void);
typedef const void **(*sf_il2cpp_domain_get_assemblies_fn)(void *, size_t *);
typedef void *(*sf_il2cpp_assembly_get_image_fn)(const void *);
typedef void *(*sf_il2cpp_class_from_name_fn)(void *, const char *,
                                              const char *);
typedef void *(*sf_il2cpp_class_get_method_fn)(void *, const char *, int);
typedef void *(*sf_il2cpp_runtime_invoke_fn)(void *, void *, void **, void **);
typedef void *(*sf_il2cpp_object_unbox_fn)(void *);

static sf_il2cpp_domain_get_fn q_domain_get;
static sf_il2cpp_domain_get_assemblies_fn q_domain_get_assemblies;
static sf_il2cpp_assembly_get_image_fn q_assembly_get_image;
static sf_il2cpp_class_from_name_fn q_class_from_name;
static sf_il2cpp_class_get_method_fn q_class_get_method;
static sf_il2cpp_runtime_invoke_fn q_runtime_invoke;
static sf_il2cpp_object_unbox_fn q_object_unbox;

nxgl_quality_level sf_quality_recommend(long mem_total_kb)
{
    if (mem_total_kb > 0 && mem_total_kb <= 1536L * 1024L)
        return NXGL_QUALITY_LOW;
    if (mem_total_kb > 0 && mem_total_kb <= 3072L * 1024L)
        return NXGL_QUALITY_MEDIUM;
    if (mem_total_kb > 0)
        return NXGL_QUALITY_HIGH;
    return NXGL_QUALITY_MEDIUM;
}

int sf_quality_profile_knobs(nxgl_quality_level level,
                             sf_quality_knobs *knobs)
{
    if (!knobs)
        return -1;
    switch (level) {
    case NXGL_QUALITY_LOW:
        /* Global memory tier proven by the same upload policy used by the
         * Brotato port: static RGBA/ETC2 artwork is halved, while dynamic
         * targets and one/two-channel glyph atlases remain untouched. */
        knobs->tex_half_min = 1025;
        knobs->etc1_enabled = 1;
        knobs->etc1_min = 64;
        return 0;
    case NXGL_QUALITY_MEDIUM:
        /* The official Unity Low preset supplies the global mip limit. */
        knobs->tex_half_min = 0;
        knobs->etc1_enabled = 1;
        knobs->etc1_min = 256;
        return 0;
    case NXGL_QUALITY_HIGH:
        knobs->tex_half_min = 0;
        knobs->etc1_enabled = 0;
        knobs->etc1_min = 256;
        return 0;
    case NXGL_QUALITY_AUTO:
    case NXGL_QUALITY_LEVEL_COUNT:
    default:
        return -1;
    }
}

static long read_mem_total_kb(void)
{
    FILE *file = fopen("/proc/meminfo", "r");
    char line[256];
    long value = -1;
    if (!file)
        return -1;
    while (fgets(line, sizeof line, file)) {
        if (sscanf(line, "MemTotal: %ld kB", &value) == 1)
            break;
    }
    fclose(file);
    return value;
}

static void report_unknown_key(const char *key, size_t key_len, void *unused)
{
    (void)unused;
    fprintf(stderr, "[sf/quality] chave desconhecida rejeitada: %.*s\n",
            (int)key_len, key);
}

/* Return 0 for a valid document, 1 for an absent document, -1 for a rejected
 * document. In both fallback cases settings remains at the canonical AUTO
 * defaults, so malformed owner input never reaches the graphics adapter. */
static int read_settings(const char *gamedir, nxcompat_settings *settings)
{
    static const char safe_default[] =
        "# NEXTOS_SETTINGS/1\nlanguage=auto\nquality=auto\n";
    char path[1200];
    char buffer[NXCOMPAT_SETTINGS_MAX_BYTES];
    struct stat before;
    struct stat after;
    size_t used = 0;
    int fd;

    (void)nxcompat_settings_parse(safe_default, sizeof safe_default - 1,
                                  settings, NULL, NULL);
    if (!gamedir || snprintf(path, sizeof path, "%s/NEXTOSSETTINGS.txt",
                             gamedir) >= (int)sizeof path) {
        fprintf(stderr, "[sf/quality] caminho de settings invalido; usando auto\n");
        return -1;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT) {
            fprintf(stderr, "[sf/quality] NEXTOSSETTINGS.txt ausente; usando auto\n");
            return 1;
        }
        fprintf(stderr, "[sf/quality] settings inacessivel (%s); usando auto\n",
                strerror(errno));
        return -1;
    }
    if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_size < 0 ||
        before.st_size > (off_t)sizeof buffer) {
        fprintf(stderr, "[sf/quality] settings inseguro/grande; usando auto\n");
        close(fd);
        return -1;
    }
    while (used < (size_t)before.st_size) {
        ssize_t got = read(fd, buffer + used, (size_t)before.st_size - used);
        if (got <= 0) {
            fprintf(stderr, "[sf/quality] leitura incompleta; usando auto\n");
            close(fd);
            return -1;
        }
        used += (size_t)got;
    }
    if (fstat(fd, &after) != 0 || after.st_dev != before.st_dev ||
        after.st_ino != before.st_ino || after.st_size != before.st_size) {
        fprintf(stderr, "[sf/quality] settings mudou durante leitura; usando auto\n");
        close(fd);
        return -1;
    }
    close(fd);
    if (nxcompat_settings_parse(buffer, used, settings, report_unknown_key,
                                NULL) != 0) {
        fprintf(stderr, "[sf/quality] settings rejeitado (fail-closed); usando auto\n");
        return -1;
    }
    return 0;
}

static void emit_receipt(nxgl_quality_stage stage)
{
    char receipt[192];
    if (nxgl_quality_receipt(&quality_state, stage, receipt,
                             sizeof receipt))
        fprintf(stderr, "%s\n", receipt);
}

static int resolve_runtime_api(void)
{
    nx_mod *module = nx_find_mod("libil2cpp.so");
    if (!module)
        return 0;
    q_domain_get = (void *)nx_lookup_in(module, "il2cpp_domain_get");
    q_domain_get_assemblies =
        (void *)nx_lookup_in(module, "il2cpp_domain_get_assemblies");
    q_assembly_get_image =
        (void *)nx_lookup_in(module, "il2cpp_assembly_get_image");
    q_class_from_name =
        (void *)nx_lookup_in(module, "il2cpp_class_from_name");
    q_class_get_method =
        (void *)nx_lookup_in(module, "il2cpp_class_get_method_from_name");
    q_runtime_invoke =
        (void *)nx_lookup_in(module, "il2cpp_runtime_invoke");
    q_object_unbox =
        (void *)nx_lookup_in(module, "il2cpp_object_unbox");
    return q_domain_get && q_domain_get_assemblies && q_assembly_get_image &&
           q_class_from_name && q_class_get_method && q_runtime_invoke &&
           q_object_unbox;
}

static void *find_runtime_class(const char *namespaze, const char *name)
{
    void *domain = q_domain_get();
    size_t count = 0;
    const void **assemblies = domain
        ? q_domain_get_assemblies(domain, &count) : NULL;
    for (size_t i = 0; assemblies && i < count; i++) {
        void *image = q_assembly_get_image(assemblies[i]);
        void *klass = image
            ? q_class_from_name(image, namespaze, name) : NULL;
        if (klass)
            return klass;
    }
    return NULL;
}

static int invoke_quality_get(void *method, int *level)
{
    void *exception = NULL;
    void *boxed = method
        ? q_runtime_invoke(method, NULL, NULL, &exception) : NULL;
    if (exception || !boxed)
        return 0;
    void *value = q_object_unbox(boxed);
    if (!value)
        return 0;
    *level = *(int *)value;
    return 1;
}

/* Sally Face ships exactly two Unity presets in data.unity3d: Low (index 0,
 * globalTextureMipmapLimit=1, lower shadows/LOD) and High (index 1, original).
 * Low/medium select Low; high selects High. Use the public managed API, never
 * an RVA patch or an asset rewrite. */
static int apply_unity_preset(unsigned long frame)
{
    void *klass;
    void *set_level;
    void *get_level;
    void *exception = NULL;
    int before = -1;
    int after = -1;
    int target = quality_state.resolved == NXGL_QUALITY_HIGH ? 1 : 0;
    const char *preset = target == 1 ? "High" : "Low";
    uint8_t expensive = 1;
    void *arguments[2] = { &target, &expensive };

    if (runtime_attempts >= 10)
        return -1;
    if (frame < 300 || frame < runtime_next_attempt)
        return 0;
    runtime_attempts++;
    runtime_next_attempt = frame + 60;
    if (!resolve_runtime_api())
        goto retry;
    klass = find_runtime_class("UnityEngine", "QualitySettings");
    set_level = klass
        ? q_class_get_method(klass, "SetQualityLevel", 2) : NULL;
    get_level = klass
        ? q_class_get_method(klass, "GetQualityLevel", 0) : NULL;
    if (!set_level || !get_level)
        goto retry;
    (void)invoke_quality_get(get_level, &before);
    q_runtime_invoke(set_level, NULL, arguments, &exception);
    if (exception || !invoke_quality_get(get_level, &after) || after != target)
        goto retry;
    fprintf(stderr,
            "QUALITY-RUNTIME: resolved=%s unity_preset=%s index=%d "
            "before=%d after=%d applied=1\n",
            nxgl_quality_name(quality_state.resolved), preset, target,
            before, after);
    runtime_applied = 1;
    runtime_applied_frame = frame;
    return 1;

retry:
    if (runtime_attempts == 1 || runtime_attempts == 10)
        fprintf(stderr,
                "[sf/quality] preset Unity ainda indisponivel tentativa=%u/10\n",
                runtime_attempts);
    return runtime_attempts >= 10 ? -1 : 0;
}

void sf_quality_init(const char *gamedir)
{
    nxcompat_settings settings;
    sf_quality_knobs knobs;
    long mem_total_kb = read_mem_total_kb();
    nxgl_quality_level recommended = sf_quality_recommend(mem_total_kb);
    nxgl_quality_level requested;
    char value[32];

    (void)read_settings(gamedir, &settings);
    requested = nxgl_quality_parse(settings.quality);
    if (nxgl_quality_state_init(&quality_state, requested, recommended) != 0 ||
        sf_quality_profile_knobs(quality_state.resolved, &knobs) != 0) {
        fprintf(stderr, "[sf/quality] falha interna ao resolver perfil\n");
        exit(1);
    }
    emit_receipt(NXGL_QUALITY_STAGE_RESOLVE);

    snprintf(value, sizeof value, "%d", knobs.tex_half_min);
    setenv("SF_TEX_HALF_MIN", value, 1);
    snprintf(value, sizeof value, "%d", knobs.etc1_enabled);
    setenv("SF_ETC1", value, 1);
    snprintf(value, sizeof value, "%d", knobs.etc1_min);
    setenv("SF_ETC1_MIN", value, 1);

    if (nxgl_quality_state_apply(&quality_state) != 0) {
        fprintf(stderr, "[sf/quality] falha interna ao aplicar perfil\n");
        exit(1);
    }
    quality_initialized = 1;
    quality_ready = 0;
    runtime_applied = 0;
    runtime_applied_frame = 0;
    runtime_attempts = 0;
    runtime_next_attempt = 0;
    emit_receipt(NXGL_QUALITY_STAGE_APPLY);
    fprintf(stderr,
            "[sf/quality] ram=%ldMB recomendado=%s aplicado=%s "
            "half_min=%d etc1=%d etc1_min=%d\n",
            mem_total_kb > 0 ? mem_total_kb / 1024 : -1,
            nxgl_quality_name(recommended),
            nxgl_quality_name(quality_state.resolved), knobs.tex_half_min,
            knobs.etc1_enabled, knobs.etc1_min);
}

void sf_quality_tick(unsigned long frame)
{
    if (!quality_initialized || quality_ready)
        return;
    if (!runtime_applied &&
        apply_unity_preset(frame) < 0)
        return;
    /* All profiles become ready on the first frame drawn after the matching
     * official Unity preset was verified. */
    if (runtime_applied && frame > runtime_applied_frame &&
        nxgl_quality_state_ready(&quality_state) == 0) {
        quality_ready = 1;
        emit_receipt(NXGL_QUALITY_STAGE_READY);
    }
}
