/* Bomb Chicken PlayerPrefs.GetString IL2CPP overload bridge. */

#ifndef BC_PLAYERPREFS_FIX_H
#define BC_PLAYERPREFS_FIX_H

#include <stddef.h>

typedef int (*bc_playerprefs_store_get_fn)(const char *key, char *out,
                                            size_t size);
typedef void *(*bc_playerprefs_string_new_fn)(const char *text);
typedef void (*bc_playerprefs_string_ascii_fn)(void *string, char *out,
                                                size_t size);

void bc_playerprefs_fix_configure(bc_playerprefs_store_get_fn store_get,
                                  bc_playerprefs_string_new_fn string_new,
                                  bc_playerprefs_string_ascii_fn string_ascii);

/* IL2CPP static ABI: arguments declared by C# followed by MethodInfo*. */
void *bc_playerprefs_getstring_default_hook(void *key_string,
                                            void *default_string,
                                            void *method);
void *bc_playerprefs_getstring_empty_hook(void *key_string, void *method);

#endif /* BC_PLAYERPREFS_FIX_H */
