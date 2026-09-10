#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "contract.h"

static const char *const quirk_env[] = {
    "BC_CURSOR",
    "BC_PAUSE_NATIVE",
    "BC_NONNULL_OBJECT_FALLBACK",
    "BC_OPAQUE_BACKBUFFER",
    "BC_PROGRESS_FIX",
    "BC_FORCE_STENCIL",
    "BC_RESUME_XY",
    "SDL_GAMECONTROLLER_USE_BUTTON_LABELS",
};

static void fail(const char *message)
{
    fprintf(stderr, "contract token test failed: %s\n", message);
    exit(1);
}

static void clear_environment(void)
{
    for (size_t i = 0; i < sizeof quirk_env / sizeof *quirk_env; i++)
        unsetenv(quirk_env[i]);
    unsetenv("NXCOMPAT_ENABLED_QUIRKS");
}

static void require_value(const char *name, const char *expected)
{
    const char *actual = getenv(name);
    if (!actual || strcmp(actual, expected) != 0)
        fail(name);
}

static void require_absent(const char *name)
{
    if (getenv(name))
        fail(name);
}

static void test_exact_parser(void)
{
    const char *list = "alpha\nbeta\r\ngamma";

    if (!bc_contract_has_token(list, "alpha") ||
        !bc_contract_has_token(list, "beta") ||
        !bc_contract_has_token(list, "gamma"))
        fail("exact LF/CRLF tokens were not accepted");
    if (bc_contract_has_token(list, "alp") ||
        bc_contract_has_token(list, "betax") ||
        bc_contract_has_token("prefix-gamma", "gamma") ||
        bc_contract_has_token("alpha,beta", "alpha") ||
        bc_contract_has_token(list, "") ||
        bc_contract_has_token(NULL, "alpha"))
        fail("substring, comma or empty token was accepted");
}

static void test_declared_subset(void)
{
    clear_environment();
    if (setenv("NXCOMPAT_ENABLED_QUIRKS",
               "game.bombchicken.native-pause-v44\r\n"
               "game.bombchicken.stencil8-v44\n"
               "game.bombchicken.present-alpha-one-extra", 1) != 0)
        fail("setenv subset");
    bc_apply_declared_contract();

    require_value("BC_PAUSE_NATIVE", "1");
    require_value("BC_FORCE_STENCIL", "1");
    require_value("BC_RESUME_XY", "1113,61");
    require_value("SDL_GAMECONTROLLER_USE_BUTTON_LABELS", "0");
    require_absent("BC_CURSOR");
    require_absent("BC_NONNULL_OBJECT_FALLBACK");
    require_absent("BC_OPAQUE_BACKBUFFER");
    require_absent("BC_PROGRESS_FIX");
}

static void test_manual_override_is_preserved(void)
{
    clear_environment();
    if (setenv("BC_CURSOR", "diagnostic", 1) != 0)
        fail("setenv manual override");
    bc_apply_declared_contract();
    require_value("BC_CURSOR", "diagnostic");
    require_value("SDL_GAMECONTROLLER_USE_BUTTON_LABELS", "0");
    require_absent("BC_PAUSE_NATIVE");
}

static void test_all_declared_tokens(void)
{
    static const char list[] =
        "game.bombchicken.menu-cursor-v44\n"
        "game.bombchicken.native-pause-v44\n"
        "game.bombchicken.nonnull-play-games-v44\n"
        "game.bombchicken.present-alpha-one\n"
        "game.bombchicken.progress-parser-v44\n"
        "game.bombchicken.stencil8-v44\n";

    clear_environment();
    if (setenv("NXCOMPAT_ENABLED_QUIRKS", list, 1) != 0)
        fail("setenv complete list");
    bc_apply_declared_contract();
    for (size_t i = 0; i < 6; i++)
        require_value(quirk_env[i], "1");
}

int main(void)
{
    test_exact_parser();
    test_declared_subset();
    test_manual_override_is_preserved();
    test_all_declared_tokens();
    clear_environment();
    puts("bombchicken exact contract-token tests passed");
    return 0;
}
