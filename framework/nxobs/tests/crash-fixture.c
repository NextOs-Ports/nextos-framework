/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxobs_crash.h"

#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void crash_now(void)
{
    const char *configured = getenv("NXOBS_FIXTURE_BAD_ADDRESS");
    uintptr_t address = configured == NULL ? 1U :
        (uintptr_t)strtoull(configured, NULL, 0);
    volatile uint32_t *invalid = (volatile uint32_t *)address;
    *invalid = 0x4e584f42U;
}

int main(int argc, char **argv)
{
    struct nxobs_crash_config config;

    if (argc != 3 ||
        (strcmp(argv[2], "early") != 0 && strcmp(argv[2], "late") != 0 &&
         strcmp(argv[2], "abort") != 0 && strcmp(argv[2], "clean") != 0)) {
        return 64;
    }
    config.runtime_dir = argv[1];
    config.port_id = "nxobsfixture";
    config.initial_phase = "runtime-start";
    config.initial_provider = "synthetic-provider";
    if (nxobs_crash_install(&config) != 0) {
        perror("nxobs_crash_install");
        return 65;
    }
    if (strcmp(argv[2], "late") == 0) {
        nxobs_crash_set_phase("draw");
        nxobs_crash_set_frame(42);
        nxobs_crash_set_last_asset("ferryman-atlas");
        nxobs_crash_set_last_graphics_call("glDrawElements");
        nxobs_crash_set_provider("wayland-gles2");
    }
    if (strcmp(argv[2], "clean") == 0) {
        nxobs_crash_uninstall();
        return 0;
    }
    if (strcmp(argv[2], "abort") == 0) {
        nxobs_crash_set_phase("abort-boundary");
        raise(SIGABRT);
        return 67;
    }
    crash_now();
    nxobs_crash_uninstall();
    return 66;
}
