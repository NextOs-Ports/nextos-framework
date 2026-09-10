/* SPDX-License-Identifier: GPL-3.0-only */
/* V4-PRE-02A: resolve optional SDL symbols (post-2.0.4 floor) strictly inside
 * the SDL provider the process already loaded.  See nxcompat_sdl_optional.h
 * for the public contract. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* dladdr, RTLD_DEFAULT */
#endif

#include "nxcompat_sdl_optional.h"

#include <dlfcn.h>
#include <string.h>

#define NXCOMPAT_SDL_OPTIONAL_VENDOR_SYMBOL "SDL_JoystickGetVendor"
#define NXCOMPAT_SDL_OPTIONAL_PRODUCT_SYMBOL "SDL_JoystickGetProduct"

/* dlsym returns object pointers while the resolved entry points are
 * functions; the union keeps the conversion out of undefined territory. */
typedef union nxcompat_sdl_optional_cast {
  void *object;
  nxcompat_sdl_optional_u16_fn u16_fn;
} nxcompat_sdl_optional_cast;

static int nxcompat_sdl_optional_options_valid(
    const nxcompat_sdl_optional_options *options) {
  if (!options || options->api_version != NXCOMPAT_API_VERSION_V2 ||
      options->struct_size < sizeof(*options))
    return 0;
  if (!options->resolver && !options->provider_anchor)
    return 0;
  return 1;
}

static int nxcompat_sdl_optional_name_valid(const char *symbol_name) {
  return symbol_name && strncmp(symbol_name, "SDL_", 4) == 0 &&
         symbol_name[4] != '\0';
}

/* Default resolver: look the name up ONLY in the namespace of modules the
 * process has already loaded (dlsym over the global scope), then require the
 * candidate to live in the exact module that provides the caller's anchor
 * symbol (dladdr base identity).  Nothing is ever opened, searched or loaded
 * by library name or path.  A name whose global binding resolves to another
 * module — absent, foreign or shadowing the provider — reads as safe absence
 * with zero metadata; another implementation is never chosen. */
static nxcompat_result_code
nxcompat_sdl_optional_default_lookup(const void *provider_anchor,
                                     const char *symbol_name, void **address) {
  Dl_info anchor_info;
  Dl_info symbol_info;
  void *candidate;

  memset(&anchor_info, 0, sizeof(anchor_info));
  /* dladdr takes a non-const pointer on some libcs; the cast never writes. */
  if (dladdr((void *)(uintptr_t)provider_anchor, &anchor_info) == 0 ||
      !anchor_info.dli_fbase)
    return NXCOMPAT_FAILED;

  candidate = dlsym(RTLD_DEFAULT, symbol_name);
  if (candidate) {
    memset(&symbol_info, 0, sizeof(symbol_info));
    if (dladdr(candidate, &symbol_info) == 0 ||
        symbol_info.dli_fbase != anchor_info.dli_fbase) {
      /* The global binding of the name lives outside the SDL that provides
       * the anchor.  Identity absent, different or ambiguous means safe
       * absence, never a lookup by name/path and never another module. */
      candidate = NULL;
    }
  }

  *address = candidate;
  return NXCOMPAT_OK;
}

static nxcompat_result_code
nxcompat_sdl_optional_lookup(const nxcompat_sdl_optional_options *options,
                             const char *symbol_name, void **address) {
  *address = NULL;
  if (options->resolver) {
    *address = options->resolver(options->resolver_userdata, symbol_name);
    return NXCOMPAT_OK;
  }
  return nxcompat_sdl_optional_default_lookup(options->provider_anchor,
                                              symbol_name, address);
}

nxcompat_result_code
nxcompat_sdl_optional_symbol(const nxcompat_sdl_optional_options *options,
                             const char *symbol_name, void **address) {
  if (address)
    *address = NULL;
  if (!address || !nxcompat_sdl_optional_options_valid(options) ||
      !nxcompat_sdl_optional_name_valid(symbol_name))
    return NXCOMPAT_INVALID;
  return nxcompat_sdl_optional_lookup(options, symbol_name, address);
}

nxcompat_result_code nxcompat_sdl_optional_resolve_joystick_api(
    const nxcompat_sdl_optional_options *options,
    nxcompat_sdl_optional_joystick_api *api) {
  nxcompat_sdl_optional_cast vendor;
  nxcompat_sdl_optional_cast product;
  nxcompat_result_code outcome;

  if (!api)
    return NXCOMPAT_INVALID;
  memset(api, 0, sizeof(*api));
  if (!nxcompat_sdl_optional_options_valid(options))
    return NXCOMPAT_INVALID;

  vendor.object = NULL;
  product.object = NULL;
  outcome = nxcompat_sdl_optional_lookup(
      options, NXCOMPAT_SDL_OPTIONAL_VENDOR_SYMBOL, &vendor.object);
  if (outcome != NXCOMPAT_OK)
    return outcome;
  outcome = nxcompat_sdl_optional_lookup(
      options, NXCOMPAT_SDL_OPTIONAL_PRODUCT_SYMBOL, &product.object);
  if (outcome != NXCOMPAT_OK)
    return outcome;

  api->api_version = NXCOMPAT_API_VERSION_V2;
  api->struct_size = sizeof(*api);
  api->get_vendor = vendor.u16_fn;
  api->get_product = product.u16_fn;
  return NXCOMPAT_OK;
}

static int nxcompat_sdl_optional_joystick_api_valid(
    const nxcompat_sdl_optional_joystick_api *api) {
  return api && api->api_version == NXCOMPAT_API_VERSION_V2 &&
         api->struct_size >= sizeof(*api);
}

nxcompat_result_code nxcompat_sdl_optional_joystick_ids(
    const nxcompat_sdl_optional_joystick_api *api, void *sdl_joystick,
    nxcompat_sdl_joystick_ids *ids) {
  if (!ids)
    return NXCOMPAT_INVALID;
  memset(ids, 0, sizeof(*ids));
  ids->api_version = NXCOMPAT_API_VERSION_V2;
  ids->struct_size = sizeof(*ids);
  if (!nxcompat_sdl_optional_joystick_api_valid(api) || !sdl_joystick)
    return NXCOMPAT_INVALID;

  if (api->get_vendor) {
    ids->vendor = api->get_vendor(sdl_joystick);
    ids->vendor_known = 1;
  }
  if (api->get_product) {
    ids->product = api->get_product(sdl_joystick);
    ids->product_known = 1;
  }
  return NXCOMPAT_OK;
}
