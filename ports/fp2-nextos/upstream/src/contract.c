#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "contract.h"

int fp2_contract_has_token(const char *list, const char *token)
{
    size_t wanted;
    const char *cursor;

    if (!list || !token || !*token)
        return 0;
    wanted = strlen(token);
    cursor = list;
    while (*cursor) {
        const char *end = strchr(cursor, '\n');
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        if (length && cursor[length - 1] == '\r')
            length--;
        if (length == wanted && memcmp(cursor, token, wanted) == 0)
            return 1;
        if (!end)
            break;
        cursor = end + 1;
    }
    return 0;
}

int fp2_apply_declared_contract(void)
{
    /* The host-driven Unity frame loop cannot use the incremental collector.
     * Override any global CFW preference before Unity starts. */
    if (setenv("GC_DISABLE_INCREMENTAL", "1", 1) != 0)
        return -1;

    /* Android gamepads are positional. Preserve A/B/X/Y positions instead of
     * applying a host's Nintendo-style display labels. */
    if (setenv("SDL_GAMECONTROLLER_USE_BUTTON_LABELS", "0", 1) != 0)
        return -1;

    /* No game-specific quirk is enabled until its behavior is observed on the
     * exact Freedom Planet 2 build and recorded in this port's contract. */
    return 0;
}
