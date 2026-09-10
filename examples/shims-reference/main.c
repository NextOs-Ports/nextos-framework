/* SPDX-License-Identifier: GPL-3.0-only */
/* NextOS — bounded host contract probe, no game or device access. */
#include "shims.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static void *thread_probe(void *context) {
    int *passed = context;
    const struct nx_demo_symbol *symbol = nx_demo_resolve("__errno", NX_DEMO_ERRNO_POINTER);
    if (symbol == NULL)
        return NULL;
    *symbol->function.errno_pointer() = ERANGE;
    *passed = errno == ERANGE;
    return NULL;
}

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", #condition); \
        return 1; \
    } \
} while (0)

int main(void) {
    const struct nx_demo_symbol *error_symbol;
    const struct nx_demo_symbol *log_symbol;
    pthread_t thread;
    int thread_passed = 0;
    char property[64];

    error_symbol = nx_demo_resolve("__errno", NX_DEMO_ERRNO_POINTER);
    log_symbol = nx_demo_resolve("__android_log_write", NX_DEMO_LOG_WRITE);
    REQUIRE(error_symbol != NULL && log_symbol != NULL);
    REQUIRE(nx_demo_resolve("__errno", NX_DEMO_LOG_WRITE) == NULL);
    REQUIRE(nx_demo_resolve("unknown_required_import", NX_DEMO_LOG_WRITE) == NULL);
    REQUIRE(nx_demo_resolve(NULL, NX_DEMO_LOG_WRITE) == NULL);
    *error_symbol->function.errno_pointer() = EDOM;
    REQUIRE(pthread_create(&thread, NULL, thread_probe, &thread_passed) == 0);
    REQUIRE(pthread_join(thread, NULL) == 0);
    REQUIRE(thread_passed && errno == EDOM);
    REQUIRE(nx_demo_property("demo.name", property, sizeof(property)) == 1);
    REQUIRE(strcmp(property, "NextOS shim reference") == 0);
    REQUIRE(nx_demo_property("ro.build.version.sdk", property, sizeof(property)) == 0);
    REQUIRE(property[0] == '\0');
    REQUIRE(nx_demo_property("demo.name", property, 2) == -1);
    REQUIRE(log_symbol->function.log_write(4, "contracts", "explicit implementations only") == 1);
    REQUIRE(log_symbol->function.log_write(4, NULL, "invalid") == -EINVAL);
    puts("PASS: typed resolution, unknown rejection, TLS errno, bounded property and logging");
    return 0;
}
