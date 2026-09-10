/*
 * PlayerPrefs.GetString repair kept separate from input.c so the two IL2CPP
 * overload ABIs can be exercised on the host.  A missing clean-save key is
 * the critical case: GetString(key) has no defaultValue argument and must
 * manufacture the managed empty string promised by Unity.
 */

#include "playerprefs_fix.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bc_playerprefs_store_get_fn prefs_store_get;
static bc_playerprefs_string_new_fn prefs_string_new;
static bc_playerprefs_string_ascii_fn prefs_string_ascii;

void bc_playerprefs_fix_configure(bc_playerprefs_store_get_fn store_get,
                                  bc_playerprefs_string_new_fn string_new,
                                  bc_playerprefs_string_ascii_fn string_ascii)
{
    prefs_store_get = store_get;
    prefs_string_new = string_new;
    prefs_string_ascii = string_ascii;
}

static int hex_value(unsigned char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    value = (unsigned char)tolower(value);
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    return -1;
}

static void url_decode(char *text)
{
    char *out = text;
    for (char *in = text; *in; in++) {
        int high = hex_value((unsigned char)in[1]);
        int low = high >= 0 ? hex_value((unsigned char)in[2]) : -1;
        if (in[0] == '%' && high >= 0 && low >= 0) {
            *out++ = (char)((high << 4) | low);
            in += 2;
        } else {
            *out++ = *in;
        }
    }
    *out = '\0';
}

static int encode_spaces(const char *key, char *encoded, size_t size)
{
    size_t used = 0;
    int changed = 0;
    if (!size)
        return 0;
    for (; *key; key++) {
        if (*key == ' ') {
            if (used + 3 >= size)
                return 0;
            memcpy(encoded + used, "%20", 3);
            used += 3;
            changed = 1;
        } else {
            if (used + 1 >= size)
                return 0;
            encoded[used++] = *key;
        }
    }
    encoded[used] = '\0';
    return changed;
}

static void *read_managed_value(void *key_string)
{
    if (!prefs_store_get || !prefs_string_new || !prefs_string_ascii)
        return NULL;

    char key[256] = "";
    char value[8192] = "";
    prefs_string_ascii(key_string, key, sizeof key);
    key[sizeof key - 1] = '\0';
    if (!key[0])
        return NULL;

    int found = prefs_store_get(key, value, sizeof value);
    if (!found) {
        /* Chaves historicas foram gravadas urlencodadas (music%20enabled). */
        char encoded[512];
        if (encode_spaces(key, encoded, sizeof encoded))
            found = prefs_store_get(encoded, value, sizeof value);
    }
    if (!found)
        return NULL;

    value[sizeof value - 1] = '\0';
    url_decode(value);
    return prefs_string_new(value);
}

void *bc_playerprefs_getstring_default_hook(void *key_string,
                                            void *default_string,
                                            void *method)
{
    (void)method;
    void *managed = read_managed_value(key_string);
    return managed ? managed : default_string;
}

void *bc_playerprefs_getstring_empty_hook(void *key_string, void *method)
{
    (void)method;
    void *managed = read_managed_value(key_string);
    if (managed)
        return managed;
    return prefs_string_new ? prefs_string_new("") : NULL;
}
