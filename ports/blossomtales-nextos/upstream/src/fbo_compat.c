/* SPDX-License-Identifier: GPL-3.0-only */
#include "fbo_compat.h"

#include <string.h>

#define BT_FBO_MARKER "BT-FBO-COMPAT/1"
#define BT_FBO_EXTENSION "GL_EXT_framebuffer_object"

typedef struct bt_fbo_alias {
    const char *extension_name;
    const char *core_name;
} bt_fbo_alias;

/* Exact closure loaded by MonoGame.OpenGL.GL.LoadFrameBufferObjectEXTEntryPoints
 * in the supported Blossom Tales assembly. */
static const bt_fbo_alias aliases[] = {
    {"glGenRenderbuffersEXT", "glGenRenderbuffers"},
    {"glBindRenderbufferEXT", "glBindRenderbuffer"},
    {"glDeleteRenderbuffersEXT", "glDeleteRenderbuffers"},
    {"glGenFramebuffersEXT", "glGenFramebuffers"},
    {"glBindFramebufferEXT", "glBindFramebuffer"},
    {"glDeleteFramebuffersEXT", "glDeleteFramebuffers"},
    {"glFramebufferTexture2DEXT", "glFramebufferTexture2D"},
    {"glFramebufferRenderbufferEXT", "glFramebufferRenderbuffer"},
    {"glRenderbufferStorageEXT", "glRenderbufferStorage"},
    {"glRenderbufferStorageMultisampleEXT", "glRenderbufferStorageMultisample"},
    {"glGenerateMipmapEXT", "glGenerateMipmap"},
    {"glBlitFramebufferEXT", "glBlitFramebuffer"},
    {"glCheckFramebufferStatusEXT", "glCheckFramebufferStatus"},
};

const char *bt_fbo_compat_marker(void)
{
    return BT_FBO_MARKER;
}

static int has_token(const char *text, const char *token)
{
    size_t length;
    const char *at;

    if (!text || !token || !*token)
        return 0;
    length = strlen(token);
    at = text;
    while ((at = strstr(at, token)) != NULL) {
        int left = at == text || at[-1] == ' ';
        int right = at[length] == '\0' || at[length] == ' ';
        if (left && right)
            return 1;
        at += length;
    }
    return 0;
}

const char *bt_fbo_compat_core_name(const char *requested)
{
    size_t i;

    if (!requested)
        return NULL;
    for (i = 0; i < sizeof aliases / sizeof aliases[0]; ++i)
        if (strcmp(requested, aliases[i].extension_name) == 0)
            return aliases[i].core_name;
    return NULL;
}

int bt_fbo_compat_supported(bt_fbo_resolver_fn resolve, void *userdata)
{
    size_t i;

    if (!resolve)
        return 0;
    for (i = 0; i < sizeof aliases / sizeof aliases[0]; ++i)
        if (!resolve(aliases[i].core_name, userdata))
            return 0;
    return 1;
}

void *bt_fbo_compat_resolve(const char *requested, void *existing,
                            bt_fbo_resolver_fn resolve, void *userdata)
{
    const char *core;

    if (existing)
        return existing;
    core = bt_fbo_compat_core_name(requested);
    if (!core || !bt_fbo_compat_supported(resolve, userdata))
        return NULL;
    return resolve(core, userdata);
}

int bt_fbo_compat_extensions(const char *base, char *out, size_t cap,
                             bt_fbo_resolver_fn resolve, void *userdata)
{
    size_t base_length, token_length, needed;
    int append;

    if (out && cap)
        out[0] = '\0';
    if (!base || !out || cap == 0)
        return -1;

    base_length = strlen(base);
    append = !has_token(base, BT_FBO_EXTENSION) &&
             bt_fbo_compat_supported(resolve, userdata);
    token_length = append ? strlen(BT_FBO_EXTENSION) : 0u;
    needed = base_length + (append && base_length ? 1u : 0u) + token_length + 1u;
    if (needed > cap)
        return -1;

    memcpy(out, base, base_length);
    if (append) {
        size_t at = base_length;
        if (at)
            out[at++] = ' ';
        memcpy(out + at, BT_FBO_EXTENSION, token_length);
        at += token_length;
        out[at] = '\0';
    } else {
        out[base_length] = '\0';
    }
    return append ? 1 : 0;
}
