#ifndef FP2_UNITY6_SHADER_H
#define FP2_UNITY6_SHADER_H

#include <stddef.h>

enum fp2_shader_stage {
    FP2_SHADER_STAGE_VERTEX = 1,
    FP2_SHADER_STAGE_FRAGMENT = 2,
};

enum fp2_shader_translate_result {
    FP2_SHADER_PASSTHROUGH = 0,
    FP2_SHADER_TRANSLATED = 1,
    FP2_SHADER_UNSUPPORTED = -1,
    FP2_SHADER_NO_MEMORY = -2,
};

typedef struct {
    int location;
    char name[64];
} fp2_attrib_binding;

typedef struct {
    fp2_attrib_binding bindings[32];
    size_t count;
} fp2_shader_attribs;

/* Translate one already-selected Unity/HLSLcc shader stage.  The returned
 * source is owned by the caller and must be freed with free().  A rejected
 * construct is named in reason; the runtime then lets the original source
 * fail visibly instead of silently compiling a semantically wrong shader. */
int fp2_unity6_translate_shader(enum fp2_shader_stage stage,
                               const char *source, size_t source_len,
                               char **translated, size_t *translated_len,
                               char *reason, size_t reason_size);

int fp2_unity6_translate_shader_ex(enum fp2_shader_stage stage,
                                  const char *source, size_t source_len,
                                  char **translated, size_t *translated_len,
                                  fp2_shader_attribs *attribs,
                                  char *reason, size_t reason_size);

#endif
