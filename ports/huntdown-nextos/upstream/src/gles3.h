#ifndef HUNTDOWN_GLES3_H
#define HUNTDOWN_GLES3_H

void *hd_gles3_sym(const char *name);
void *hd_gles3_override_sym(const char *name);
void *hd_gles3_fallback(const char *name);
unsigned hd_gles3_texture_target(unsigned target);
unsigned hd_gles3_texture_format(unsigned format);
void hd_gles3_texture_bound(unsigned active_texture, unsigned target,
                            unsigned texture);

#endif
