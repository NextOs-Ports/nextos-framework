/* SPDX-License-Identifier: GPL-3.0-only */
#include "fbo_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const core[] = {
    "glGenRenderbuffers", "glBindRenderbuffer", "glDeleteRenderbuffers",
    "glGenFramebuffers", "glBindFramebuffer", "glDeleteFramebuffers",
    "glFramebufferTexture2D", "glFramebufferRenderbuffer",
    "glRenderbufferStorage", "glRenderbufferStorageMultisample",
    "glGenerateMipmap", "glBlitFramebuffer", "glCheckFramebufferStatus",
};
static const char *const ext[] = {
    "glGenRenderbuffersEXT", "glBindRenderbufferEXT",
    "glDeleteRenderbuffersEXT", "glGenFramebuffersEXT",
    "glBindFramebufferEXT", "glDeleteFramebuffersEXT",
    "glFramebufferTexture2DEXT", "glFramebufferRenderbufferEXT",
    "glRenderbufferStorageEXT", "glRenderbufferStorageMultisampleEXT",
    "glGenerateMipmapEXT", "glBlitFramebufferEXT",
    "glCheckFramebufferStatusEXT",
};
static int symbols[sizeof core / sizeof core[0]];
static int missing = -1;

static void fail(const char *message)
{
    fprintf(stderr, "test_fbo_compat: %s\n", message);
    exit(1);
}

#define CHECK(condition, message) do { if (!(condition)) fail(message); } while (0)

static void *resolve(const char *name, void *userdata)
{
    size_t i;
    (void)userdata;
    for (i = 0; i < sizeof core / sizeof core[0]; ++i) {
        if (strcmp(name, core[i]) == 0)
            return (int)i == missing ? NULL : &symbols[i];
    }
    return NULL;
}

static int token_count(const char *text, const char *token)
{
    int count = 0;
    size_t length = strlen(token);
    while ((text = strstr(text, token)) != NULL) {
        ++count;
        text += length;
    }
    return count;
}

int main(void)
{
    char extensions[160];
    char small[8] = "dirty";
    void *native = &symbols[0];
    void *aliased;

    CHECK(strcmp(bt_fbo_compat_marker(), "BT-FBO-COMPAT/1") == 0,
          "marker changed");
    CHECK(bt_fbo_compat_supported(resolve, NULL),
          "complete GLES core closure not recognized");
    CHECK(bt_fbo_compat_extensions("GL_EXT_texture_rg", extensions,
                                   sizeof extensions, resolve, NULL) == 1,
          "extension was not appended");
    CHECK(strcmp(extensions,
                 "GL_EXT_texture_rg GL_EXT_framebuffer_object") == 0,
          "wrong extension string");
    for (size_t i = 0; i < sizeof core / sizeof core[0]; ++i) {
        CHECK(strcmp(bt_fbo_compat_core_name(ext[i]), core[i]) == 0,
              "EXT/core alias table drifted");
        CHECK(bt_fbo_compat_resolve(ext[i], NULL, resolve, NULL) ==
                  resolve(core[i], NULL),
              "an EXT spelling did not resolve to its core function");
    }
    aliased = bt_fbo_compat_resolve("glBindFramebufferEXT", NULL,
                                    resolve, NULL);
    CHECK(aliased == resolve("glBindFramebuffer", NULL),
          "EXT spelling did not resolve to its core function");
    CHECK(bt_fbo_compat_resolve("glOtherEXT", native, resolve, NULL) == native,
          "existing unrelated symbol was changed");
    CHECK(bt_fbo_compat_resolve("glOtherEXT", NULL, resolve, NULL) == NULL,
          "unknown symbol was invented");

    CHECK(bt_fbo_compat_extensions(
              "GL_EXT_framebuffer_object GL_EXT_texture_rg", extensions,
              sizeof extensions, resolve, NULL) == 0,
          "existing extension should be preserved");
    CHECK(token_count(extensions, "GL_EXT_framebuffer_object") == 1,
          "existing extension was duplicated");

    missing = 11; /* glBlitFramebuffer */
    CHECK(!bt_fbo_compat_supported(resolve, NULL),
          "partial closure was accepted");
    CHECK(bt_fbo_compat_extensions("GL_EXT_texture_rg", extensions,
                                   sizeof extensions, resolve, NULL) == 0,
          "partial closure advertised the extension");
    CHECK(strcmp(extensions, "GL_EXT_texture_rg") == 0,
          "partial closure changed the extension string");
    CHECK(bt_fbo_compat_resolve("glBindFramebufferEXT", NULL,
                                resolve, NULL) == NULL,
          "partial closure exposed an alias");

    missing = -1;
    CHECK(bt_fbo_compat_extensions("GL_EXT_texture_rg", small,
                                   sizeof small, resolve, NULL) == -1,
          "short output buffer was accepted");
    CHECK(small[0] == '\0', "short output buffer did not fail closed");

    puts("BLOSSOM FBO COMPAT: PASS closure=13 aliases=13 fail_closed=1");
    return 0;
}
