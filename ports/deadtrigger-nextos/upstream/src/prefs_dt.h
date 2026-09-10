#ifndef DEADTRIGGER_PREFS_DT_H
#define DEADTRIGGER_PREFS_DT_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Persistent typed storage behind Android SharedPreferences.  The JNI layer
 * owns Java object identity; this module only implements the key/value store.
 */
int dt_prefs_ready(void);
int dt_prefs_contains(const char *key);

char *dt_prefs_get_string_copy(const char *key);
int dt_prefs_get_int(const char *key, int32_t *value);
int dt_prefs_get_float(const char *key, float *value);
int dt_prefs_get_bool(const char *key, int *value);
int dt_prefs_get_long(const char *key, int64_t *value);

int dt_prefs_set_string(const char *key, const char *value);
int dt_prefs_set_int(const char *key, int32_t value);
int dt_prefs_set_float(const char *key, float value);
int dt_prefs_set_bool(const char *key, int value);
int dt_prefs_set_long(const char *key, int64_t value);
int dt_prefs_remove(const char *key);
int dt_prefs_clear(void);

/* Android commit() and apply() both publish the staged map durably here. */
int dt_prefs_flush(const char *reason);

#endif
