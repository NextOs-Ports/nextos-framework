#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "nxcompat_settings.h"
#include "nxgl_quality.h"
#include "nx_elf.h"
#include "quality.h"

/* Runtime IL2CPP lookup is device-tested; pure host profile tests do not load
 * guest objects, so provide the absent loader boundary explicitly. */
nx_mod *nx_find_mod(const char *soname)
{
    (void)soname;
    return NULL;
}

void *nx_lookup_in(nx_mod *module, const char *symbol)
{
    (void)module;
    (void)symbol;
    return NULL;
}

static nxgl_quality_level parse_profile(const char *value, long mem_kb)
{
    char document[128];
    nxcompat_settings settings;
    nxgl_quality_state state;
    int length = snprintf(document, sizeof document,
                          "# NEXTOS_SETTINGS/1\nlanguage=auto\nquality=%s\n",
                          value);
    assert(length > 0 && (size_t)length < sizeof document);
    assert(nxcompat_settings_parse(document, (size_t)length, &settings,
                                   NULL, NULL) == 0);
    assert(nxgl_quality_state_init(&state,
                                   nxgl_quality_parse(settings.quality),
                                   sf_quality_recommend(mem_kb)) == 0);
    return state.resolved;
}

int main(void)
{
    sf_quality_knobs low, medium, high;
    nxcompat_settings rejected;
    static const char bad[] =
        "# NEXTOS_SETTINGS/1\nlanguage=auto\nquality=ultra\n";

    assert(parse_profile("auto", 1024L * 1024L) == NXGL_QUALITY_LOW);
    assert(parse_profile("auto", 2048L * 1024L) == NXGL_QUALITY_MEDIUM);
    assert(parse_profile("auto", 4096L * 1024L) == NXGL_QUALITY_HIGH);
    assert(parse_profile("low", 4096L * 1024L) == NXGL_QUALITY_LOW);
    assert(parse_profile("medium", 1024L * 1024L) == NXGL_QUALITY_MEDIUM);
    assert(parse_profile("high", 1024L * 1024L) == NXGL_QUALITY_HIGH);

    assert(nxcompat_settings_parse(bad, sizeof bad - 1, &rejected,
                                   NULL, NULL) != 0);
    assert(strcmp(rejected.quality, "auto") == 0);
    assert(nxgl_quality_resolve(nxgl_quality_parse(rejected.quality),
                                sf_quality_recommend(1024L * 1024L)) ==
           NXGL_QUALITY_LOW);

    assert(sf_quality_profile_knobs(NXGL_QUALITY_LOW, &low) == 0);
    assert(sf_quality_profile_knobs(NXGL_QUALITY_MEDIUM, &medium) == 0);
    assert(sf_quality_profile_knobs(NXGL_QUALITY_HIGH, &high) == 0);
    assert(low.tex_half_min == 1025 && low.etc1_enabled == 1 &&
           low.etc1_min == 64);
    assert(medium.tex_half_min == 0 && medium.etc1_enabled == 1 &&
           medium.etc1_min == 256);
    assert(high.tex_half_min == 0 && high.etc1_enabled == 0);
    assert(sf_quality_profile_knobs(NXGL_QUALITY_AUTO, &low) != 0);

    puts("quality profile tests: OK");
    return 0;
}
