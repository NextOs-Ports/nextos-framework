#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "contract.h"

int bc_contract_has_token(const char *list, const char *token)
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

void bc_apply_declared_contract(void)
{
    static const struct {
        const char *quirk;
        const char *environment;
    } switches[] = {
        { "game.bombchicken.menu-cursor-v44", "BC_CURSOR" },
        { "game.bombchicken.native-pause-v44", "BC_PAUSE_NATIVE" },
        { "game.bombchicken.nonnull-play-games-v44",
          "BC_NONNULL_OBJECT_FALLBACK" },
        { "game.bombchicken.present-alpha-one",
          "BC_OPAQUE_BACKBUFFER" },
        { "game.bombchicken.progress-parser-v44", "BC_PROGRESS_FIX" },
        { "game.bombchicken.stencil8-v44", "BC_FORCE_STENCIL" },
        { "adapter.gl-provider-probe-init-reexec", "BC_GLFIX" },
        { "game.bombchicken.playerprefs-getstring-v44",
          "BC_PREFS_STRING_FIX" },
    };
    const char *enabled = getenv("NXCOMPAT_ENABLED_QUIRKS");

    for (size_t i = 0; i < sizeof switches / sizeof *switches; i++) {
        if (bc_contract_has_token(enabled, switches[i].quirk)) {
            setenv(switches[i].environment, "1", 0);
            fprintf(stderr, "[bc] enabled manifest quirk: %s\n",
                    switches[i].quirk);
        }
    }

    /* Android gamepads are positional. Preserve A/B/X/Y positions instead of
     * applying a host's Nintendo-style display labels. */
    setenv("SDL_GAMECONTROLLER_USE_BUTTON_LABELS", "0", 0);
    if (bc_contract_has_token(enabled,
                              "game.bombchicken.native-pause-v44"))
        setenv("BC_RESUME_XY", "1113,61", 0);
}
