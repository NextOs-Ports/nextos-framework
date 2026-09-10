/* SPDX-License-Identifier: GPL-3.0-only */
/* Default-resolver proof against a really loaded fixture provider.  The same
 * source is compiled once per scenario:
 *   - floor204: provider without Vendor/Product (plus a loaded FOREIGN
 *     library exporting poisoned versions of both names, which must never be
 *     selected);
 *   - sdl206: provider with both symbols;
 *   - partial: provider with SDL_JoystickGetVendor only.
 * Expectations arrive as compile definitions FIXTURE_EXPECT_VENDOR,
 * FIXTURE_EXPECT_PRODUCT, FIXTURE_VENDOR_VALUE and FIXTURE_PRODUCT_VALUE. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "nxcompat_sdl_optional.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
      return 1;                                                                \
    }                                                                          \
  } while (0)

#define FIXTURE_POISON 0xdeadu

/* Baseline anchor exported by every fixture provider.  The parameter type is
 * irrelevant for taking the address. */
void SDL_GetVersion(void *version);

#if FIXTURE_WITH_FOREIGN
/* Keeps the foreign library in DT_NEEDED (and therefore loaded) even under
 * --as-needed linking, so the poison scenario is real. */
int fake_foreign_present(void);
#endif

/* Anchor living in THIS executable, i.e. in a module that is not the
 * provider.  Symbols the provider exports globally must then read as absent:
 * anchor and candidate from different modules never match. */
static void local_anchor(void) {}

int main(void) {
  nxcompat_sdl_optional_options options;
  nxcompat_sdl_optional_joystick_api api;
  nxcompat_sdl_joystick_ids ids;
  void *address = NULL;
  int cookie = 0;
  int repeat;

#if FIXTURE_WITH_FOREIGN
  CHECK(fake_foreign_present() == 1);
#endif

  memset(&options, 0, sizeof(options));
  options.api_version = NXCOMPAT_API_VERSION_V2;
  options.struct_size = sizeof(options);
  options.provider_anchor = (const void *)SDL_GetVersion;

  for (repeat = 0; repeat < 2; ++repeat) {
    CHECK(nxcompat_sdl_optional_resolve_joystick_api(&options, &api) ==
          NXCOMPAT_OK);
#if FIXTURE_EXPECT_VENDOR
    CHECK(api.get_vendor != NULL);
#else
    CHECK(api.get_vendor == NULL);
#endif
#if FIXTURE_EXPECT_PRODUCT
    CHECK(api.get_product != NULL);
#else
    CHECK(api.get_product == NULL);
#endif

    CHECK(nxcompat_sdl_optional_joystick_ids(&api, &cookie, &ids) ==
          NXCOMPAT_OK);
#if FIXTURE_EXPECT_VENDOR
    CHECK(ids.vendor_known == 1 && ids.vendor == FIXTURE_VENDOR_VALUE);
#else
    CHECK(ids.vendor_known == 0 && ids.vendor == 0);
#endif
#if FIXTURE_EXPECT_PRODUCT
    CHECK(ids.product_known == 1 && ids.product == FIXTURE_PRODUCT_VALUE);
#else
    CHECK(ids.product_known == 0 && ids.product == 0);
#endif
    /* A poisoned value from the foreign library is a broken provider-identity
     * contract, regardless of the expectation flags above. */
    CHECK(ids.vendor != FIXTURE_POISON && ids.product != FIXTURE_POISON);
  }

  CHECK(nxcompat_sdl_optional_symbol(&options, "SDL_JoystickGetVendor",
                                     &address) == NXCOMPAT_OK);
#if FIXTURE_EXPECT_VENDOR
  CHECK(address != NULL);
#else
  CHECK(address == NULL);
#endif
  CHECK(nxcompat_sdl_optional_symbol(&options, "SDL_JoystickGetProduct",
                                     &address) == NXCOMPAT_OK);
#if FIXTURE_EXPECT_PRODUCT
  CHECK(address != NULL);
#else
  CHECK(address == NULL);
#endif

  /* Different-module identity: with an anchor from this executable, every
   * optional symbol — including the ones this scenario's provider really
   * exports in the global namespace — must read as safe absence. */
  options.provider_anchor = (const void *)local_anchor;
  CHECK(nxcompat_sdl_optional_resolve_joystick_api(&options, &api) ==
        NXCOMPAT_OK);
  CHECK(api.get_vendor == NULL && api.get_product == NULL);
  CHECK(nxcompat_sdl_optional_joystick_ids(&api, &cookie, &ids) ==
        NXCOMPAT_OK);
  CHECK(ids.vendor_known == 0 && ids.vendor == 0 &&
        ids.product_known == 0 && ids.product == 0);
  CHECK(nxcompat_sdl_optional_symbol(&options, "SDL_JoystickGetVendor",
                                     &address) == NXCOMPAT_OK);
  CHECK(address == NULL);

  printf("nxcompat-sdl-optional-fixture: PASS\n");
  return 0;
}
