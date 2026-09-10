#include "playerprefs_fix.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef void *(*getstring_default_abi)(void *, void *, void *);
typedef void *(*getstring_empty_abi)(void *, void *);

static const struct {
    const char *key;
    const char *value;
} prefs[] = {
    { "START_WORLD", "Skull%2DWorld" },
    { "Progress", "Skull%2DWorld%2CR1G1%2D1" },
    { "music%20enabled", "false" },
};

static char managed_pool[16][8192];
static size_t managed_count;

static int fake_store_get(const char *key, char *out, size_t size)
{
    for (size_t i = 0; i < sizeof prefs / sizeof *prefs; i++) {
        if (strcmp(key, prefs[i].key) == 0) {
            if (!size)
                return 0;
            snprintf(out, size, "%s", prefs[i].value);
            return 1;
        }
    }
    return 0;
}

static void *fake_string_new(const char *text)
{
    if (managed_count >= sizeof managed_pool / sizeof *managed_pool)
        return NULL;
    snprintf(managed_pool[managed_count], sizeof managed_pool[managed_count],
             "%s", text);
    return managed_pool[managed_count++];
}

static void fake_string_ascii(void *string, char *out, size_t size)
{
    if (!size)
        return;
    snprintf(out, size, "%s", string ? (const char *)string : "");
}

static int check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "playerprefs overload test failed: %s\n", message);
        return 0;
    }
    return 1;
}

int main(void)
{
    /* These assignments fail at compile time if either hook ABI drifts. */
    getstring_default_abi get_with_default =
        bc_playerprefs_getstring_default_hook;
    getstring_empty_abi get_with_empty =
        bc_playerprefs_getstring_empty_hook;
    void *method_sentinel = (void *)(uintptr_t)0x12345678u;
    char fallback[] = "fallback";

    bc_playerprefs_fix_configure(fake_store_get, fake_string_new,
                                 fake_string_ascii);

    void *result = get_with_default("START_WORLD", fallback, method_sentinel);
    if (!check(result != fallback && strcmp(result, "Skull-World") == 0,
               "explicit-default overload did not decode a stored value"))
        return 1;

    result = get_with_default("NEVER_SAVED", fallback, method_sentinel);
    if (!check(result == fallback,
               "explicit-default overload did not preserve its managed default"))
        return 1;

    result = get_with_empty("NEVER_SAVED", method_sentinel);
    if (!check(result && result != method_sentinel && strcmp(result, "") == 0,
               "clean-save key did not return a managed empty string"))
        return 1;

    result = get_with_empty("Progress", method_sentinel);
    if (!check(result && strcmp(result, "Skull-World,R1G1-1") == 0,
               "one-argument overload did not decode a stored value"))
        return 1;

    result = get_with_empty("music enabled", method_sentinel);
    if (!check(result && strcmp(result, "false") == 0,
               "legacy URL-encoded key fallback failed"))
        return 1;

    bc_playerprefs_fix_configure(NULL, NULL, NULL);
    result = get_with_default("START_WORLD", fallback, method_sentinel);
    if (!check(result == fallback, "unconfigured default overload is not safe"))
        return 1;
    result = get_with_empty("START_WORLD", method_sentinel);
    if (!check(result == NULL, "unconfigured empty overload is not safe"))
        return 1;

    puts("playerprefs GetString overload/clean-save tests passed");
    return 0;
}
