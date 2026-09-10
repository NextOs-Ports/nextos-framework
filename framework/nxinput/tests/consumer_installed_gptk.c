/* SPDX-License-Identifier: GPL-3.0-only
 * Proves the INSTALLED nxinput-gptk library is usable by an adapter: includes
 * only the installed headers, links only the installed archive, and calls the
 * V3 symbols (parse + dispatcher + motion). */
#include <nxinput_gptk.h>
#include <nxinput_gptk_loader.h>
#include <nxinput_gptk_motion.h>
#include <nxinput_exit_chord.h>
#include <stdio.h>
#include <string.h>

static int sink_hits;
static void sink(void *u, const char *action, int pressed, float v)
{ (void)u; (void)action; (void)pressed; (void)v; sink_hits++; }

int main(void)
{
    static const char text[] =
        "format = NEXTOS_CONTROLLERS/1\n"
        "port = consumer\n"
        "[menu]\nA = ui.confirm\nRIGHT_STICK = cursor.move\n"
        "[gameplay]\nA = player.jump\nB = player.action\n";
    nxinput_gptk map;
    char err[128];
    if (nxinput_gptk_parse(text, strlen(text), &map, err, sizeof err) != 0) {
        printf("consumer: parse failed: %s\n", err);
        return 1;
    }
    const char *a = nxinput_gptk_action(&map, NXINPUT_GPTK_CONTEXT_GAMEPLAY,
                                        NXINPUT_GPTK_A);
    if (a == NULL || strcmp(a, "player.jump") != 0) {
        printf("consumer: unexpected action %s\n", a ? a : "(null)");
        return 1;
    }
    nxinput_gptk_dispatcher d;
    nxinput_gptk_source_guard guard;
    nxinput_gptk_dispatcher_init(&d, &map);
    nxinput_gptk_source_guard_init(&guard, &d);
    nxinput_gptk_dispatcher_register(&d, "player.jump", sink, NULL);
    nxinput_gptk_dispatcher_set_context(&d, NXINPUT_GPTK_CONTEXT_GAMEPLAY);
    nxinput_gptk_dispatcher_set_primary_mask(&d, &guard,
                                              1u << NXINPUT_GPTK_A);
    nxinput_gptk_dispatcher_feed_source(&d, &guard,
                                        NXINPUT_GPTK_SOURCE_PRIMARY,
                                        NXINPUT_GPTK_A, 1, 1.0f);
    nxinput_gptk_dispatcher_feed_source(&d, &guard,
                                        NXINPUT_GPTK_SOURCE_PRIMARY,
                                        NXINPUT_GPTK_A, 0, 0.0f);
    nxinput_gptk_cursor_tuning ct;
    nxinput_gptk_cursor_tuning_defaults(&ct);
    nxinput_gptk_cursor_state cs;
    nxinput_gptk_cursor_state_reset(&cs, 0.0f, 0.0f);
    nxinput_gptk_cursor_step(&ct, 1.0f, 0.0f, 1.0f / 60.0f, 1280, 720, &cs);
    if (sink_hits < 2) { printf("consumer: sink not delivered\n"); return 1; }
    /* V3 blocker 7: the stick-vector path is reachable from the installed
     * lib and the double-read guard is derived from the live mapping. */
    nxinput_gptk_dispatcher_configure_motion(&d, 1280, 720);
    nxinput_gptk_dispatcher_set_context(&d, NXINPUT_GPTK_CONTEXT_MENU);
    if (nxinput_gptk_dispatcher_physical_suppressed(&d) != 1) {
        printf("consumer: cursor stick not suppressed\n");
        return 1;
    }
    /* Per-control mask (blocker 7): only the right stick (the cursor one) is
     * owned; the unmapped left stick must stay native. */
    if (nxinput_gptk_dispatcher_control_suppressed(&d, NXINPUT_GPTK_RIGHT_STICK) != 1 ||
        nxinput_gptk_dispatcher_control_suppressed(&d, NXINPUT_GPTK_LEFT_STICK) != 0 ||
        (nxinput_gptk_dispatcher_suppressed_mask(&d) &
         (1u << NXINPUT_GPTK_LEFT_STICK)) != 0u) {
        printf("consumer: per-stick suppression mask wrong (left stolen?)\n");
        return 1;
    }
    nxinput_gptk_dispatcher_feed_stick(&d, NXINPUT_GPTK_RIGHT_STICK,
                                       1.0f, 0.0f, 0.5f);
    /* Public additive symbols are present in the installed archive. */
    if (strcmp(nxinput_gptk_load_source_name(NXINPUT_GPTK_LOAD_OWNER),
               "owner") != 0) {
        printf("consumer: loader symbol unavailable\n");
        return 1;
    }
    nxinput_exit_chord chord;
    nxinput_exit_chord_init(&chord, 1u);
    if (!nxinput_exit_chord_update(&chord, 1, 1) ||
        !nxinput_exit_chord_consume(&chord)) {
        printf("consumer: neutral chord symbol unavailable\n");
        return 1;
    }
    printf("nxinput installed-consumer OK: sink_hits=%d cursor_x=%.1f\n",
           sink_hits, (double)cs.x);
    return 0;
}
