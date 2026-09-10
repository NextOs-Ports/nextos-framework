#ifndef HUNTDOWN_UNITY6_SHADER_H
#define HUNTDOWN_UNITY6_SHADER_H

#include <stddef.h>

enum hd_shader_stage {
    HD_SHADER_STAGE_VERTEX = 1,
    HD_SHADER_STAGE_FRAGMENT = 2,
};

enum hd_shader_translate_result {
    HD_SHADER_PASSTHROUGH = 0,
    HD_SHADER_TRANSLATED = 1,
    HD_SHADER_UNSUPPORTED = -1,
    HD_SHADER_NO_MEMORY = -2,
};

typedef struct {
    int location;
    char name[64];
} hd_attrib_binding;

typedef struct {
    hd_attrib_binding bindings[32];
    size_t count;
} hd_shader_attribs;

/* Translate one already-selected Unity/HLSLcc shader stage.  The returned
 * source is owned by the caller and must be freed with free().  A rejected
 * construct is named in reason; the runtime then lets the original source
 * fail visibly instead of silently compiling a semantically wrong shader. */
int hd_unity6_translate_shader(enum hd_shader_stage stage,
                               const char *source, size_t source_len,
                               char **translated, size_t *translated_len,
                               char *reason, size_t reason_size);

int hd_unity6_translate_shader_ex(enum hd_shader_stage stage,
                                  const char *source, size_t source_len,
                                  char **translated, size_t *translated_len,
                                  hd_shader_attribs *attribs,
                                  char *reason, size_t reason_size);

#endif
