/* SPDX-License-Identifier: GPL-3.0-only */
/* Hermetic V4-PRE-02A gate: no SDL is linked or loaded here.  Providers are
 * modeled with caller resolvers (SDL 2.0.4 without Vendor/Product, SDL 2.0.6+
 * with both, one symbol only) and the default in-process resolver runs
 * against a process that has no SDL at all, which must read as absence, never
 * as a crash or an invented VID/PID. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "nxcompat_sdl_optional.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
      return 1;                                                                \
    }                                                                          \
  } while (0)

#define FAKE_VENDOR 0x1209u
#define FAKE_PRODUCT 0x4004u

static uint16_t fake_get_vendor(void *joystick) {
  (void)joystick;
  return FAKE_VENDOR;
}

static uint16_t fake_get_product(void *joystick) {
  (void)joystick;
  return FAKE_PRODUCT;
}

typedef struct fake_provider {
  int has_vendor;
  int has_product;
  unsigned lookups;
  unsigned unexpected_names;
} fake_provider;

static void *fake_resolver(void *userdata, const char *name) {
  fake_provider *provider = userdata;
  union {
    nxcompat_sdl_optional_u16_fn function;
    void *object;
  } cast;

  ++provider->lookups;
  if (strcmp(name, "SDL_JoystickGetVendor") == 0 && provider->has_vendor) {
    cast.function = fake_get_vendor;
    return cast.object;
  }
  if (strcmp(name, "SDL_JoystickGetProduct") == 0 && provider->has_product) {
    cast.function = fake_get_product;
    return cast.object;
  }
  if (strcmp(name, "SDL_JoystickGetVendor") != 0 &&
      strcmp(name, "SDL_JoystickGetProduct") != 0)
    ++provider->unexpected_names;
  return NULL;
}

static void options_with_resolver(nxcompat_sdl_optional_options *options,
                                  fake_provider *provider) {
  memset(options, 0, sizeof(*options));
  options->api_version = NXCOMPAT_API_VERSION_V2;
  options->struct_size = sizeof(*options);
  options->resolver = fake_resolver;
  options->resolver_userdata = provider;
}

/* Anchor for the default resolver in a process without SDL: any address that
 * dladdr can attribute to a loaded module. */
static void anchor_function(void) {}

static void options_with_anchor(nxcompat_sdl_optional_options *options) {
  memset(options, 0, sizeof(*options));
  options->api_version = NXCOMPAT_API_VERSION_V2;
  options->struct_size = sizeof(*options);
  options->provider_anchor = (const void *)anchor_function;
}

static int check_ids(const nxcompat_sdl_optional_joystick_api *api,
                     int expect_vendor_known, uint16_t expect_vendor,
                     int expect_product_known, uint16_t expect_product) {
  nxcompat_sdl_joystick_ids ids;
  int cookie = 0;

  memset(&ids, 0xa5, sizeof(ids));
  if (nxcompat_sdl_optional_joystick_ids(api, &cookie, &ids) != NXCOMPAT_OK)
    return 0;
  return ids.api_version == NXCOMPAT_API_VERSION_V2 &&
         ids.struct_size == sizeof(ids) &&
         ids.vendor_known == expect_vendor_known &&
         ids.vendor == expect_vendor &&
         ids.product_known == expect_product_known &&
         ids.product == expect_product;
}

static int worker_failures;
static pthread_mutex_t worker_lock = PTHREAD_MUTEX_INITIALIZER;

static void *worker(void *userdata) {
  fake_provider provider;
  nxcompat_sdl_optional_options options;
  nxcompat_sdl_optional_joystick_api api;
  int iteration;
  int failed = 0;

  (void)userdata;
  for (iteration = 0; iteration < 512 && !failed; ++iteration) {
    memset(&provider, 0, sizeof(provider));
    provider.has_vendor = 1;
    provider.has_product = 1;
    options_with_resolver(&options, &provider);
    if (nxcompat_sdl_optional_resolve_joystick_api(&options, &api) !=
            NXCOMPAT_OK ||
        !check_ids(&api, 1, FAKE_VENDOR, 1, FAKE_PRODUCT))
      failed = 1;

    options_with_anchor(&options);
    if (nxcompat_sdl_optional_resolve_joystick_api(&options, &api) !=
            NXCOMPAT_OK ||
        api.get_vendor != NULL || api.get_product != NULL ||
        !check_ids(&api, 0, 0, 0, 0))
      failed = 1;
  }
  if (failed) {
    pthread_mutex_lock(&worker_lock);
    ++worker_failures;
    pthread_mutex_unlock(&worker_lock);
  }
  return NULL;
}

int main(void) {
  fake_provider provider;
  nxcompat_sdl_optional_options options;
  nxcompat_sdl_optional_joystick_api api;
  nxcompat_sdl_joystick_ids ids;
  void *address;
  pthread_t threads[8];
  int cookie = 0;
  int index;

  /* Malformed requests fail closed and leave zeroed outputs. */
  memset(&provider, 0, sizeof(provider));
  options_with_resolver(&options, &provider);
  memset(&api, 0xa5, sizeof(api));
  CHECK(nxcompat_sdl_optional_resolve_joystick_api(NULL, &api) ==
        NXCOMPAT_INVALID);
  CHECK(api.get_vendor == NULL && api.get_product == NULL);
  CHECK(nxcompat_sdl_optional_resolve_joystick_api(&options, NULL) ==
        NXCOMPAT_INVALID);
  options.api_version = NXCOMPAT_API_VERSION_V1;
  CHECK(nxcompat_sdl_optional_resolve_joystick_api(&options, &api) ==
        NXCOMPAT_INVALID);
  options_with_resolver(&options, &provider);
  options.struct_size = sizeof(options) - 1;
  CHECK(nxcompat_sdl_optional_resolve_joystick_api(&options, &api) ==
        NXCOMPAT_INVALID);

  /* A NULL resolver requires an anchor: the provider must be identifiable. */
  memset(&options, 0, sizeof(options));
  options.api_version = NXCOMPAT_API_VERSION_V2;
  options.struct_size = sizeof(options);
  CHECK(nxcompat_sdl_optional_resolve_joystick_api(&options, &api) ==
        NXCOMPAT_INVALID);
  CHECK(nxcompat_sdl_optional_symbol(&options, "SDL_JoystickGetVendor",
                                     &address) == NXCOMPAT_INVALID);

  /* SDL 2.0.4-equivalent provider: both symbols absent, boot proceeds. */
  memset(&provider, 0, sizeof(provider));
  options_with_resolver(&options, &provider);
  CHECK(nxcompat_sdl_optional_resolve_joystick_api(&options, &api) ==
        NXCOMPAT_OK);
  CHECK(api.api_version == NXCOMPAT_API_VERSION_V2);
  CHECK(api.struct_size == sizeof(api));
  CHECK(api.get_vendor == NULL && api.get_product == NULL);
  CHECK(provider.lookups == 2 && provider.unexpected_names == 0);
  CHECK(check_ids(&api, 0, 0, 0, 0));

  /* SDL 2.0.6+-equivalent provider: both symbols used with exact values. */
  memset(&provider, 0, sizeof(provider));
  provider.has_vendor = 1;
  provider.has_product = 1;
  options_with_resolver(&options, &provider);
  CHECK(nxcompat_sdl_optional_resolve_joystick_api(&options, &api) ==
        NXCOMPAT_OK);
  CHECK(api.get_vendor != NULL && api.get_product != NULL);
  CHECK(check_ids(&api, 1, FAKE_VENDOR, 1, FAKE_PRODUCT));

  /* Only one of the two symbols present, in both directions. */
  memset(&provider, 0, sizeof(provider));
  provider.has_vendor = 1;
  options_with_resolver(&options, &provider);
  CHECK(nxcompat_sdl_optional_resolve_joystick_api(&options, &api) ==
        NXCOMPAT_OK);
  CHECK(api.get_vendor != NULL && api.get_product == NULL);
  CHECK(check_ids(&api, 1, FAKE_VENDOR, 0, 0));

  memset(&provider, 0, sizeof(provider));
  provider.has_product = 1;
  options_with_resolver(&options, &provider);
  CHECK(nxcompat_sdl_optional_resolve_joystick_api(&options, &api) ==
        NXCOMPAT_OK);
  CHECK(api.get_vendor == NULL && api.get_product != NULL);
  CHECK(check_ids(&api, 0, 0, 1, FAKE_PRODUCT));

  /* Generic symbol lookup: prefix and output pointer are enforced. */
  memset(&provider, 0, sizeof(provider));
  provider.has_vendor = 1;
  options_with_resolver(&options, &provider);
  address = &cookie;
  CHECK(nxcompat_sdl_optional_symbol(&options, "SDL_JoystickGetVendor",
                                     &address) == NXCOMPAT_OK);
  CHECK(address != NULL);
  CHECK(nxcompat_sdl_optional_symbol(&options, "SDL_JoystickGetProduct",
                                     &address) == NXCOMPAT_OK);
  CHECK(address == NULL);
  CHECK(nxcompat_sdl_optional_symbol(&options, "JoystickGetVendor",
                                     &address) == NXCOMPAT_INVALID);
  CHECK(nxcompat_sdl_optional_symbol(&options, "SDL_", &address) ==
        NXCOMPAT_INVALID);
  CHECK(nxcompat_sdl_optional_symbol(&options, NULL, &address) ==
        NXCOMPAT_INVALID);
  CHECK(nxcompat_sdl_optional_symbol(&options, "SDL_JoystickGetVendor",
                                     NULL) == NXCOMPAT_INVALID);

  /* Default resolver in a process without SDL: identified module, absent
   * symbols, unknown/zero metadata, no crash. */
  options_with_anchor(&options);
  CHECK(nxcompat_sdl_optional_resolve_joystick_api(&options, &api) ==
        NXCOMPAT_OK);
  CHECK(api.get_vendor == NULL && api.get_product == NULL);
  CHECK(check_ids(&api, 0, 0, 0, 0));
  CHECK(nxcompat_sdl_optional_symbol(&options, "SDL_JoystickGetVendor",
                                     &address) == NXCOMPAT_OK);
  CHECK(address == NULL);

  /* Ids misuse fails closed without touching the provider. */
  memset(&provider, 0, sizeof(provider));
  provider.has_vendor = 1;
  provider.has_product = 1;
  options_with_resolver(&options, &provider);
  CHECK(nxcompat_sdl_optional_resolve_joystick_api(&options, &api) ==
        NXCOMPAT_OK);
  CHECK(nxcompat_sdl_optional_joystick_ids(&api, NULL, &ids) ==
        NXCOMPAT_INVALID);
  CHECK(ids.vendor_known == 0 && ids.product_known == 0 && ids.vendor == 0 &&
        ids.product == 0);
  CHECK(nxcompat_sdl_optional_joystick_ids(NULL, &cookie, &ids) ==
        NXCOMPAT_INVALID);
  CHECK(nxcompat_sdl_optional_joystick_ids(&api, &cookie, NULL) ==
        NXCOMPAT_INVALID);
  api.api_version = NXCOMPAT_API_VERSION_V1;
  CHECK(nxcompat_sdl_optional_joystick_ids(&api, &cookie, &ids) ==
        NXCOMPAT_INVALID);

  /* Repeated resolution is stable. */
  for (index = 0; index < 1000; ++index) {
    memset(&provider, 0, sizeof(provider));
    provider.has_vendor = 1;
    provider.has_product = 1;
    options_with_resolver(&options, &provider);
    CHECK(nxcompat_sdl_optional_resolve_joystick_api(&options, &api) ==
          NXCOMPAT_OK);
    CHECK(check_ids(&api, 1, FAKE_VENDOR, 1, FAKE_PRODUCT));
  }

  /* Concurrent resolution and queries. */
  for (index = 0; index < 8; ++index)
    CHECK(pthread_create(&threads[index], NULL, worker, NULL) == 0);
  for (index = 0; index < 8; ++index)
    CHECK(pthread_join(threads[index], NULL) == 0);
  CHECK(worker_failures == 0);

  printf("nxcompat-sdl-optional: PASS\n");
  return 0;
}
