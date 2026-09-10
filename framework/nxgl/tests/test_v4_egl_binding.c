/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * V4-GRAPHICS-03 host test. Every effect is injected, so the GLVND split
 * scenario that crashed OTR 1.0.3 on ROCKNIX, the monolithic Mali scenario,
 * the already-global path and all the negatives run without a GPU.
 */
#include "nxgl_egl_binding.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(int condition, const char *message) {
  if (!condition) {
    (void)fprintf(stderr, "nxgl_egl_binding: %s\n", message);
    exit(1);
  }
}

/* The 14 imports measured in the OTR incident. */
static const char *const kImports[] = {
  "eglGetCurrentContext", "eglGetCurrentDisplay", "eglGetCurrentSurface",
  "eglGetDisplay", "eglInitialize", "eglChooseConfig", "eglCreateContext",
  "eglCreateWindowSurface", "eglMakeCurrent", "eglSwapBuffers",
  "eglQuerySurface", "eglGetError", "eglGetConfigAttrib", "eglTerminate"
};

typedef struct fake_object {
  const char *path;
  const char *build_id;
  /* Symbols this object exports, NULL terminated. */
  const char *const *symbols;
  size_t symbol_count;
  int has_current_context;
  int already_global;
  int open_fails;
  int promote_fails;
} fake_object;

typedef struct fake_world {
  /* What SDL_GL_GetProcAddress answers for the probe. */
  int resolver_knows_probe;
  /* What dladdr reports for that address. */
  int identify_succeeds;
  const fake_object *identified;
  /* The object actually opened for that path; NULL means "same as
   * identified". Used to model a SONAME that leads somewhere else. */
  const fake_object *opened;
  /* Bookkeeping the assertions read back. */
  int open_calls;
  int close_calls;
  int promote_calls;
  int globalized;
  void *probe_address;
} fake_world;

static void *fake_resolve(void *userdata, const char *symbol) {
  fake_world *world = (fake_world *)userdata;
  if (!world->resolver_knows_probe) {
    return NULL;
  }
  if (strcmp(symbol, "eglGetCurrentContext") != 0) {
    return NULL;
  }
  return world->probe_address;
}

static int fake_identify(void *userdata, void *address,
                         nxgl_egl_dso_identity *out_identity) {
  fake_world *world = (fake_world *)userdata;
  if (!world->identify_succeeds || address != world->probe_address ||
      world->identified == NULL) {
    return 0;
  }
  (void)snprintf(out_identity->path, sizeof(out_identity->path), "%s",
                 world->identified->path);
  (void)snprintf(out_identity->build_id, sizeof(out_identity->build_id), "%s",
                 world->identified->build_id);
  out_identity->already_global = world->identified->already_global;
  return 1;
}

static const fake_object *fake_target(const fake_world *world) {
  return world->opened != NULL ? world->opened : world->identified;
}

static void *fake_open_local(void *userdata, const char *path) {
  fake_world *world = (fake_world *)userdata;
  const fake_object *target = fake_target(world);
  (void)path;
  world->open_calls += 1;
  if (target == NULL || target->open_fails) {
    return NULL;
  }
  return (void *)(uintptr_t)target;
}

static void *fake_symbol(void *userdata, void *handle, const char *name) {
  const fake_object *object = (const fake_object *)handle;
  fake_world *world = (fake_world *)userdata;
  size_t index;
  for (index = 0; index < object->symbol_count; ++index) {
    if (strcmp(object->symbols[index], name) == 0) {
      if (strcmp(name, "eglGetCurrentContext") == 0) {
        /* The identified object answers with the very address the resolver
         * produced; any other object answers with its own. */
        return object == world->identified ? world->probe_address
                                           : (void *)(uintptr_t)object;
      }
      return (void *)(uintptr_t)(0x1000u + index);
    }
  }
  return NULL;
}

static int fake_current_context(void *userdata, void *handle) {
  const fake_object *object = (const fake_object *)handle;
  (void)userdata;
  return object->has_current_context;
}

static int fake_promote_global(void *userdata, const char *path) {
  fake_world *world = (fake_world *)userdata;
  const fake_object *target = fake_target(world);
  (void)path;
  world->promote_calls += 1;
  if (target->promote_fails) {
    return 0;
  }
  world->globalized = 1;
  return 1;
}

static void fake_close(void *userdata, void *handle) {
  fake_world *world = (fake_world *)userdata;
  (void)handle;
  world->close_calls += 1;
}

static nxgl_egl_dl_ops make_ops(fake_world *world) {
  nxgl_egl_dl_ops ops;
  memset(&ops, 0, sizeof(ops));
  ops.struct_size = sizeof(ops);
  ops.userdata = world;
  ops.resolve = fake_resolve;
  ops.identify = fake_identify;
  ops.open_local = fake_open_local;
  ops.symbol = fake_symbol;
  ops.current_context = fake_current_context;
  ops.promote_global = fake_promote_global;
  ops.close = fake_close;
  return ops;
}

static nxgl_egl_binding_request make_request(size_t import_count) {
  nxgl_egl_binding_request request;
  memset(&request, 0, sizeof(request));
  request.struct_size = sizeof(request);
  request.api_version = NXGL_EGL_BINDING_API_VERSION;
  request.declared_imports = kImports;
  request.declared_import_count = import_count;
  request.guest.struct_size = sizeof(request.guest);
  (void)snprintf(request.guest.name, sizeof(request.guest.name), "libgame.so");
  (void)snprintf(request.guest.build_id, sizeof(request.guest.build_id),
                 "0d71eeb6a3ead7db0d489cfbe574f3b96014a7f4");
  return request;
}

static fake_world make_world(const fake_object *identified) {
  fake_world world;
  memset(&world, 0, sizeof(world));
  world.resolver_knows_probe = 1;
  world.identify_succeeds = 1;
  world.identified = identified;
  world.probe_address = (void *)(uintptr_t)0xE61Cu;
  return world;
}

int main(void) {
  const size_t import_count = sizeof(kImports) / sizeof(kImports[0]);
  /* GLVND split: SDL keeps libEGL.so.1 in its RTLD_LOCAL scope, so
   * dlsym(RTLD_DEFAULT) inside the so-loader finds nothing at all. */
  const fake_object glvnd = {
    "/usr/lib/aarch64-linux-gnu/libEGL.so.1",
    "fe21c660206f0866956a3277f3362195e2321973", kImports, import_count, 1, 0,
    0, 0
  };
  /* Monolithic Mali: same contract, one vendor object. */
  const fake_object mali = {
    "/usr/lib/libMali.so", "5258d644cbb023b137719901fa84acf8a8ae3da0",
    kImports, import_count, 1, 0, 0, 0
  };
  const fake_object already_global = {
    "/usr/lib/libMali.so", "5258d644cbb023b137719901fa84acf8a8ae3da0",
    kImports, import_count, 1, 1, 0, 0
  };
  static const char *const kShort[] = {
    "eglGetCurrentContext", "eglGetCurrentDisplay", "eglGetDisplay"
  };
  const fake_object incomplete = {
    "/usr/lib/libEGL-partial.so", "aa", kShort,
    sizeof(kShort) / sizeof(kShort[0]), 1, 0, 0, 0
  };
  const fake_object no_context = {
    "/usr/lib/libEGL-foreign.so", "bb", kImports, import_count, 0, 0, 0, 0
  };
  const fake_object unopenable = {
    "/usr/lib/libEGL-locked.so", "cc", kImports, import_count, 1, 0, 1, 0
  };
  const fake_object unpromotable = {
    "/usr/lib/libEGL-pinned.so", "dd", kImports, import_count, 1, 0, 0, 1
  };
  const fake_object lookalike = {
    "/usr/lib/libEGL.so.1", "ee", kImports, import_count, 1, 0, 0, 0
  };
  nxgl_egl_binding_request request = make_request(import_count);
  nxgl_egl_dl_ops ops;
  nxgl_egl_binding binding;
  fake_world world;
  char receipt[1024];
  char offender[128];
  size_t index;

  /* --- positive: GLVND split -------------------------------------------- */
  world = make_world(&glvnd);
  ops = make_ops(&world);
  check(nxgl_egl_binding_resolve(&request, &ops, &binding) ==
        NXGL_EGL_BINDING_OK, "the GLVND split provider was not bound");
  check(binding.resolved_imports == import_count &&
        binding.entry_count == import_count,
        "the binding table is incomplete");
  check(binding.context_current == 1 && binding.promoted == 1 &&
        world.globalized == 1, "the proven provider was not promoted");
  check(binding.handle != NULL, "the proven provider was not retained");
  check(strcmp(binding.provider.path,
               "/usr/lib/aarch64-linux-gnu/libEGL.so.1") == 0,
        "the receipt names the wrong provider");
  for (index = 0; index < binding.entry_count; ++index) {
    check(binding.entries[index].name == kImports[index],
          "the table lost the declared import order");
    check(binding.entries[index].address != 0,
          "the table carries a null import address");
  }
  check(nxgl_egl_binding_receipt(&binding, receipt, sizeof(receipt)) > 0,
        "the binding receipt is empty");
  check(strstr(receipt, "\"status\":\"ok\"") != NULL &&
        strstr(receipt, "\"expected\":14,\"resolved\":14") != NULL &&
        strstr(receipt, "libgame.so") != NULL,
        "the binding receipt lost its structured fields");

  /* --- positive: monolithic Mali ---------------------------------------- */
  world = make_world(&mali);
  ops = make_ops(&world);
  check(nxgl_egl_binding_resolve(&request, &ops, &binding) ==
        NXGL_EGL_BINDING_OK, "the monolithic provider was not bound");
  check(binding.promoted == 1 && world.promote_calls == 1,
        "the monolithic provider was not promoted exactly once");

  /* --- positive: the provider is already global -------------------------- */
  world = make_world(&already_global);
  ops = make_ops(&world);
  check(nxgl_egl_binding_resolve(&request, &ops, &binding) ==
        NXGL_EGL_BINDING_OK, "the already global provider was refused");
  check(binding.promoted == 0 && world.promote_calls == 0 &&
        world.globalized == 0,
        "an already global provider was promoted again");

  /* --- negative: the resolver does not know EGL at all ------------------- */
  world = make_world(&glvnd);
  world.resolver_knows_probe = 0;
  ops = make_ops(&world);
  check(nxgl_egl_binding_resolve(&request, &ops, &binding) ==
        NXGL_EGL_BINDING_NO_RESOLVER, "a missing resolver was accepted");
  check(world.open_calls == 0 && world.globalized == 0,
        "a missing resolver still touched the namespace");

  /* --- negative: dladdr cannot name the object --------------------------- */
  world = make_world(&glvnd);
  world.identify_succeeds = 0;
  ops = make_ops(&world);
  check(nxgl_egl_binding_resolve(&request, &ops, &binding) ==
        NXGL_EGL_BINDING_NO_PROVIDER_IDENTITY,
        "an unidentifiable provider was accepted");
  check(world.open_calls == 0 && world.globalized == 0,
        "an unidentifiable provider still touched the namespace");

  /* --- negative: a declared import is absent ----------------------------- */
  world = make_world(&incomplete);
  ops = make_ops(&world);
  check(nxgl_egl_binding_resolve(&request, &ops, &binding) ==
        NXGL_EGL_BINDING_MISSING_IMPORT,
        "a provider missing a declared import was accepted");
  check(strcmp(binding.failed_symbol, "eglGetCurrentSurface") == 0,
        "the missing import was not named");
  check(world.globalized == 0 && world.close_calls == 1,
        "a rejected candidate polluted the global namespace");
  check(binding.handle == NULL && binding.entry_count == 0,
        "a rejected candidate left a usable table behind");

  /* --- negative: the provider has no current context --------------------- */
  world = make_world(&no_context);
  ops = make_ops(&world);
  check(nxgl_egl_binding_resolve(&request, &ops, &binding) ==
        NXGL_EGL_BINDING_NULL_CONTEXT,
        "a foreign provider with a null context was accepted");
  check(world.globalized == 0 && world.close_calls == 1,
        "the null-context candidate polluted the global namespace");

  /* --- negative: the candidate cannot even be opened locally ------------- */
  world = make_world(&unopenable);
  ops = make_ops(&world);
  check(nxgl_egl_binding_resolve(&request, &ops, &binding) ==
        NXGL_EGL_BINDING_OPEN_FAILED, "an unopenable candidate was accepted");
  check(world.globalized == 0, "an unopenable candidate was promoted");

  /* --- negative: promotion fails after a full proof ---------------------- */
  world = make_world(&unpromotable);
  ops = make_ops(&world);
  check(nxgl_egl_binding_resolve(&request, &ops, &binding) ==
        NXGL_EGL_BINDING_PROMOTION_FAILED,
        "a failed promotion was reported as success");
  check(binding.handle == NULL && world.close_calls == 1,
        "a failed promotion kept the candidate open");

  /* --- negative: the SONAME leads to a DIFFERENT provider ---------------- */
  world = make_world(&glvnd);
  world.opened = &lookalike;
  ops = make_ops(&world);
  check(nxgl_egl_binding_resolve(&request, &ops, &binding) ==
        NXGL_EGL_BINDING_PROVIDER_MISMATCH,
        "a lookalike behind the same SONAME was accepted");
  check(world.globalized == 0 && world.close_calls == 1,
        "the lookalike polluted the global namespace");

  /* --- negative: an undeclared EGL import in the guest ------------------- */
  {
    static const char *const kGuestOk[] = {
      "eglGetCurrentContext", "eglSwapBuffers", "memcpy", "__cxa_atexit"
    };
    static const char *const kGuestExtra[] = {
      "eglGetCurrentContext", "eglCreatePbufferSurface", "memcpy"
    };
    check(nxgl_egl_binding_check_inventory(
              &request, kGuestOk, sizeof(kGuestOk) / sizeof(kGuestOk[0]),
              offender, sizeof(offender)) == NXGL_EGL_BINDING_OK,
          "a fully declared guest inventory was refused");
    check(offender[0] == '\0', "a passing inventory named an offender");
    check(nxgl_egl_binding_check_inventory(
              &request, kGuestExtra,
              sizeof(kGuestExtra) / sizeof(kGuestExtra[0]), offender,
              sizeof(offender)) == NXGL_EGL_BINDING_UNDECLARED_IMPORT,
          "an undeclared EGL import was accepted");
    check(strcmp(offender, "eglCreatePbufferSurface") == 0,
          "the undeclared EGL import was not named");
  }

  /* --- negative: malformed requests -------------------------------------- */
  {
    nxgl_egl_binding_request bad = make_request(import_count);
    static const char *const kNoProbe[] = {"eglSwapBuffers", "eglGetError"};
    static const char *const kDuplicate[] = {
      "eglGetCurrentContext", "eglGetCurrentContext"
    };
    static const char *const kNotEgl[] = {"eglGetCurrentContext", "glClear"};
    world = make_world(&glvnd);
    ops = make_ops(&world);
    bad.declared_imports = kNoProbe;
    bad.declared_import_count = 2;
    check(nxgl_egl_binding_resolve(&bad, &ops, &binding) ==
          NXGL_EGL_BINDING_INVALID_ARGUMENT,
          "an inventory without the ownership probe was accepted");
    bad.declared_imports = kDuplicate;
    check(nxgl_egl_binding_resolve(&bad, &ops, &binding) ==
          NXGL_EGL_BINDING_INVALID_ARGUMENT,
          "a duplicated declared import was accepted");
    bad.declared_imports = kNotEgl;
    check(nxgl_egl_binding_resolve(&bad, &ops, &binding) ==
          NXGL_EGL_BINDING_INVALID_ARGUMENT,
          "a non-EGL name was accepted in the EGL inventory");
    bad = make_request(import_count);
    bad.struct_size = sizeof(bad) - 1u;
    check(nxgl_egl_binding_resolve(&bad, &ops, &binding) ==
          NXGL_EGL_BINDING_INVALID_ARGUMENT,
          "a foreign request struct size was accepted");
    check(world.open_calls == 0,
          "a malformed request still opened a candidate");
  }

  (void)printf("nxgl_egl_binding: ALL PASS\n");
  return 0;
}
