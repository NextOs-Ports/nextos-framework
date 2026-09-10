/* SPDX-License-Identifier: GPL-3.0-only */
/* NextOS — typed teaching example; not a complete Android implementation. */
#ifndef NEXTOS_DEMO_SHIMS_H
#define NEXTOS_DEMO_SHIMS_H
#include <stddef.h>

enum nx_demo_signature {
    NX_DEMO_ERRNO_POINTER,
    NX_DEMO_LOG_WRITE
};

typedef int *(*nx_demo_errno_fn)(void);
typedef int (*nx_demo_log_fn)(int, const char *, const char *);

struct nx_demo_symbol {
    const char *name;
    enum nx_demo_signature signature;
    union {
        nx_demo_errno_fn errno_pointer;
        nx_demo_log_fn log_write;
    } function;
};

/* A missing name OR wrong signature returns NULL. There is no default stub. */
const struct nx_demo_symbol *nx_demo_resolve(const char *name,
                                           enum nx_demo_signature signature);

/* Example-owned query, not a drop-in Android property service.
 * 1 = supplied; 0 = absent; -1 = invalid arguments/insufficient capacity.
 * It intentionally exposes no fabricated Android version, device or GPU.
 */
int nx_demo_property(const char *name, char *out, size_t capacity);
#endif
