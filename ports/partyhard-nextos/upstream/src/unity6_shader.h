#ifndef ST_UNITY6_SHADER_H
#define ST_UNITY6_SHADER_H

#include <stddef.h>

enum st_shader_stage {
    ST_SHADER_STAGE_VERTEX = 1,
    ST_SHADER_STAGE_FRAGMENT = 2,
};

enum st_shader_translate_result {
    ST_SHADER_PASSTHROUGH = 0,
    ST_SHADER_TRANSLATED = 1,
    ST_SHADER_UNSUPPORTED = -1,
    ST_SHADER_NO_MEMORY = -2,
};

/* Translate one already-selected Unity/HLSLcc shader stage.  The returned
 * source is owned by the caller and must be freed with free().  A rejected
 * construct is named in reason; the runtime then lets the original source
 * fail visibly instead of silently compiling a semantically wrong shader. */
int st_unity6_translate_shader(enum st_shader_stage stage,
                               const char *source, size_t source_len,
                               char **translated, size_t *translated_len,
                               char *reason, size_t reason_size);

#endif
