/* Hide FP2's Android-only control canvas through the game's own visibility
 * path.  MobileControlsVisibility.Update already fades every control's
 * CanvasGroup to zero and then disables interaction and raycasts when its
 * internal "hide" predicate is true.  The handheld always has a physical
 * controller, so force only that predicate and retain the rest of the method.
 *
 * Target measured from Freedom Planet 2 1.2.8 arm64:
 *   libil2cpp.so SHA-256
 *   f373c5507f073b358d5f4fa7acdfcaab3e7a07c2ca0c85e42532816dc5002bf4
 *   MobileControlsVisibility.Update RVA 0x005f0e94
 *
 * The five-instruction guard makes a different game build fail closed instead
 * of patching an address that merely happens to exist.  FP2_SHOW_TOUCH_CONTROLS
 * is a diagnostic escape hatch and is absent from the shipped launcher.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nx_elf.h"

#define MOBILE_VISIBILITY_PREDICATE_RVA 0x005f0f88u

void fp2_hide_mobile_controls(void)
{
#ifndef FP2_RELEASE_BUILD
    const char *show = getenv("FP2_SHOW_TOUCH_CONTROLS");
    if (show && strcmp(show, "0") != 0) {
        fprintf(stderr,
                "[fp2/input] Android touch controls left visible by "
                "FP2_SHOW_TOUCH_CONTROLS\n");
        return;
    }
#endif

    nx_mod *module = nx_find_mod("libil2cpp.so");
    if (!module || MOBILE_VISIBILITY_PREDICATE_RVA + 12 > module->span)
        nx_die("cannot locate the guarded mobile-controls predicate");

    uint32_t *site = (uint32_t *)(module->base +
                                  MOBILE_VISIBILITY_PREDICATE_RVA);
    static const uint32_t expected[] = {
        0x2a080288u, /* orr w8, w20, w8       -- original hide predicate */
        0x7200011fu, /* tst w8, #1 */
        0x1e2b1c08u, /* fcsel s8, s0, s11, ne -- target alpha 0 or 1 */
    };
    if (memcmp(site, expected, sizeof expected) != 0)
        nx_die("libil2cpp mobile-controls guard does not match FP2 1.2.8");

    site[0] = 0x52800028u; /* mov w8, #1: choose the game's hidden branch */
    __builtin___clear_cache((char *)site, (char *)site + sizeof expected);
    if (site[0] != 0x52800028u)
        nx_die("failed to install the mobile-controls visibility decision");

    fprintf(stderr,
            "[fp2/input] Android touch controls hidden through the game's "
            "CanvasGroup path\n");
}
