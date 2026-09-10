#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "contract.h"

int main(void)
{
    if (setenv("SDL_GAMECONTROLLER_USE_BUTTON_LABELS", "1", 1) != 0) {
        perror("setenv");
        return 1;
    }
    if (setenv("GC_DISABLE_INCREMENTAL", "0", 1) != 0) {
        perror("setenv");
        return 1;
    }
    if (fp2_apply_declared_contract() != 0) {
        fprintf(stderr, "declared environment contract failed\n");
        return 1;
    }
    const char *value = getenv("SDL_GAMECONTROLLER_USE_BUTTON_LABELS");
    if (!value || strcmp(value, "0") != 0) {
        fprintf(stderr, "controller label contract was not authoritative\n");
        return 1;
    }
    value = getenv("GC_DISABLE_INCREMENTAL");
    if (!value || strcmp(value, "1") != 0) {
        fprintf(stderr, "Unity GC contract was not authoritative\n");
        return 1;
    }
    return 0;
}
