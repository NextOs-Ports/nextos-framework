/* SPDX-License-Identifier: GPL-3.0-only */
/* NextOS — only the documented subset is implemented. */
#include "shims.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int *errno_pointer(void) {
    return &errno;
}

/* Diagnostic backend for the __android_log_write signature.
 * This subset accepts every priority and returns 1 on delivery, negative errno
 * on invalid input or output failure. It does not implement Android filtering.
 */
static int log_write(int priority, const char *tag, const char *message) {
    if (tag == NULL || message == NULL)
        return -EINVAL;
    if (fprintf(stderr, "NextOS demo [%d] %s: %s\n", priority, tag, message) < 0)
        return -EIO;
    return 1;
}

static const struct nx_demo_symbol symbols[] = {
    { "__errno", NX_DEMO_ERRNO_POINTER, { .errno_pointer = errno_pointer } },
    { "__android_log_write", NX_DEMO_LOG_WRITE, { .log_write = log_write } }
};

const struct nx_demo_symbol *nx_demo_resolve(const char *name,
                                           enum nx_demo_signature signature) {
    size_t i;
    if (name == NULL)
        return NULL;
    for (i = 0; i < sizeof(symbols) / sizeof(symbols[0]); ++i) {
        if (symbols[i].signature == signature && strcmp(symbols[i].name, name) == 0)
            return &symbols[i];
    }
    return NULL;
}

int nx_demo_property(const char *name, char *out, size_t capacity) {
    const char value[] = "NextOS shim reference";
    if (name == NULL || out == NULL || capacity == 0)
        return -1;
    out[0] = '\0';
    if (strcmp(name, "demo.name") != 0)
        return 0;
    if (capacity < sizeof(value))
        return -1;
    memcpy(out, value, sizeof(value));
    return 1;
}
