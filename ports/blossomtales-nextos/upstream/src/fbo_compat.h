/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef BT_FBO_COMPAT_H
#define BT_FBO_COMPAT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MonoGame's Android GLES2 backend still gates framebuffer support on the
 * desktop-era GL_EXT_framebuffer_object token and then asks for the EXT
 * spellings. Mesa can expose the complete equivalent API only through the
 * GLES core spellings. This adapter advertises/aliases that capability only
 * when the whole entry-point closure used by this exact MonoGame assembly is
 * resolvable. It never invents a partially supported extension. */
typedef void *(*bt_fbo_resolver_fn)(const char *name, void *userdata);

const char *bt_fbo_compat_marker(void);
int bt_fbo_compat_supported(bt_fbo_resolver_fn resolve, void *userdata);
const char *bt_fbo_compat_core_name(const char *requested);
void *bt_fbo_compat_resolve(const char *requested, void *existing,
                            bt_fbo_resolver_fn resolve, void *userdata);

/* Copy `base` into `out` and, when the complete core closure exists, append
 * GL_EXT_framebuffer_object exactly once. Returns 1 when appended, 0 when the
 * base was copied unchanged, and -1 for invalid input/insufficient capacity.
 * On failure `out` is an empty string whenever `cap` permits it. */
int bt_fbo_compat_extensions(const char *base, char *out, size_t cap,
                             bt_fbo_resolver_fn resolve, void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* BT_FBO_COMPAT_H */
