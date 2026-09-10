/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXCOMPAT_SDL_OPTIONAL_H
#define NXCOMPAT_SDL_OPTIONAL_H

#include "nxcompat.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* V4-PRE-02A: canonical resolver for SDL APIs newer than the universal
 * SDL floor (2.0.4).
 *
 * A universal ELF may not carry a direct import of a symbol introduced after
 * the floor: on a firmware that ships SDL 2.0.4 the dynamic linker would
 * refuse to start the process.  This module resolves such symbols at runtime,
 * exclusively inside the SDL implementation the process has ALREADY loaded
 * through its regular imports: the candidate comes from a plain symbol
 * lookup over the loaded global namespace and is accepted only when it lives
 * in the exact module providing the caller's anchor symbol.  The module
 * contains no dlopen call at all — nothing is ever opened, searched or
 * loaded by library name or path, a different implementation is never
 * selected, and a name exported or shadowed by another module reads as
 * absent.  No value is ever guessed: when the loaded SDL predates a symbol,
 * the resolved pointer is NULL and the typed queries below report
 * unknown/zero metadata instead of failing boot.
 *
 * Everything here is caller-owned, has no global state, performs no caching
 * and is safe to call repeatedly and from concurrent threads.  Nothing in
 * nxcompat calls this API on its own; an engine adapter must opt in
 * explicitly. */

/* Optional caller-supplied lookup.  It must return the address `name` has in
 * the SDL provider already loaded by the process, or NULL when the provider
 * does not export it.  Hermetic fixtures use this to model SDL 2.0.4 and
 * SDL 2.0.6+ providers without loading SDL. */
typedef void *(*nxcompat_sdl_optional_resolver)(void *userdata,
                                                const char *name);

typedef struct nxcompat_sdl_optional_options {
  uint32_t api_version; /* NXCOMPAT_API_VERSION_V2 */
  size_t struct_size;
  /* Address of a baseline SDL symbol (introduced at or below the universal
   * floor) that the calling ELF already imports directly, for example
   * (const void *)SDL_GetVersion.  The default resolver uses it to identify
   * the loaded provider module; a symbol is accepted only when it lives in
   * that same module, so a foreign library exporting an SDL_-named function
   * can never be selected.  Required when `resolver` is NULL. */
  const void *provider_anchor;
  /* NULL selects the default in-process resolver described above. */
  nxcompat_sdl_optional_resolver resolver;
  void *resolver_userdata;
} nxcompat_sdl_optional_options;

/* Shape shared by SDL_JoystickGetVendor and SDL_JoystickGetProduct: a
 * Uint16 accessor over an opaque SDL_Joystick pointer. */
typedef uint16_t (*nxcompat_sdl_optional_u16_fn)(void *sdl_joystick);

typedef struct nxcompat_sdl_optional_joystick_api {
  uint32_t api_version;
  size_t struct_size;
  /* NULL when the loaded SDL predates the symbol (SDL < 2.0.6). */
  nxcompat_sdl_optional_u16_fn get_vendor;
  nxcompat_sdl_optional_u16_fn get_product;
} nxcompat_sdl_optional_joystick_api;

typedef struct nxcompat_sdl_joystick_ids {
  uint32_t api_version;
  size_t struct_size;
  /* Nonzero only when the matching symbol existed and was actually called.
   * An unknown value is reported as 0 and never invented. */
  int vendor_known;
  int product_known;
  uint16_t vendor;
  uint16_t product;
} nxcompat_sdl_joystick_ids;

/* Resolve one optional SDL symbol.  `symbol_name` must start with "SDL_".
 * On success `*address` holds the provider's address or NULL when the loaded
 * provider does not export the symbol; absence is NOT an error.  Returns
 * NXCOMPAT_INVALID for a malformed request (including a NULL resolver with a
 * NULL anchor) and NXCOMPAT_FAILED when the default resolver cannot identify
 * the module behind `provider_anchor`.  On any failure `*address` is NULL. */
nxcompat_result_code
nxcompat_sdl_optional_symbol(const nxcompat_sdl_optional_options *options,
                             const char *symbol_name, void **address);

/* Resolve SDL_JoystickGetVendor and SDL_JoystickGetProduct (SDL 2.0.6+).
 * Returns NXCOMPAT_OK even when both symbols are absent: a SDL 2.0.4
 * provider yields a valid api with both pointers NULL so boot proceeds.
 * On NXCOMPAT_INVALID/NXCOMPAT_FAILED the api is zeroed. */
nxcompat_result_code nxcompat_sdl_optional_resolve_joystick_api(
    const nxcompat_sdl_optional_options *options,
    nxcompat_sdl_optional_joystick_api *api);

/* Read the vendor/product of an opened SDL_Joystick through a resolved api.
 * A NULL function pointer (provider without the symbol) reports the matching
 * field as unknown/zero and still returns NXCOMPAT_OK; it never calls
 * anything and never invents a VID/PID.  NXCOMPAT_INVALID is returned for a
 * NULL/malformed api or ids struct or a NULL joystick, with `ids` zeroed. */
nxcompat_result_code nxcompat_sdl_optional_joystick_ids(
    const nxcompat_sdl_optional_joystick_api *api, void *sdl_joystick,
    nxcompat_sdl_joystick_ids *ids);

#ifdef __cplusplus
}
#endif

#endif
