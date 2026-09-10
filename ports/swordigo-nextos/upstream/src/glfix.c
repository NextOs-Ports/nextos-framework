/* glfix.c — GL provider repair by observed capability (no device names).
 *
 * Field evidence (dArkOS4Clone/G350, DarkOsRE R36S, dArkOSen): the firmware's
 * versioned SONAMEs are crossed — libGLESv1_CM.so.1 / libEGL.so.1 resolve to
 * Mesa builds with no driver behind them while the real Mali blob sits behind
 * the unversioned names (libGLESv1_CM.so -> libMali.so).  The binary binds by
 * SONAME, so it gets a context that accepts every call and draws nothing:
 * glGetString(GL_RENDERER) comes back NULL, audio and input keep running and
 * the panel stays black.
 *
 * The one safe, observable condition is the empty renderer string measured on
 * the real context.  A healthy Mesa (Panfrost on ROCKNIX) reports a proper
 * renderer and is left completely alone. When the condition holds and a Mali
 * blob exporting both EGL and the GLES1 entry points exists, current adapters
 * re-execute with SDL's EGL and GL provider variables bound to that exact same
 * object. Older manifests retain the narrower LD_PRELOAD path for compatibility.
 *
 * Controls:
 *   SWORDIGO_GLFIX=0            disable entirely.
 *   SWORDIGO_GLFIX_BLOB=/path   force a specific provider object.
 * A marker env var stops any re-exec loop after one attempt.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <fnmatch.h>
#include <limits.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include "glfix.h"
#include "gl_provider_policy.h"
#include "util.h"

#define GLFIX_MARKER "SWORDIGO_GLFIX_APPLIED"

static char **g_saved_argv;

void glfix_set_argv(char **argv) { g_saved_argv = argv; }

static int glfix_enabled(const char *quirk) {
  const char *mode = getenv("SWORDIGO_GLFIX");
  if (mode && mode[0] == '0') {
    debugPrintf("glfix: disabled by SWORDIGO_GLFIX=0\n");
    return 0;
  }
  if (mode && mode[0] == '1')
    return 1;
  if (!swordigo_contract_quirk_enabled(quirk)) {
    debugPrintf("glfix: disabled because manifest quirk is absent: %s\n",
                quirk);
    return 0;
  }
  return 1;
}

/* Providers already mapped in this process (the crossed or kernel-mismatched
 * ones the linker resolved).  A candidate that is the same file proved itself
 * broken by getting us here, so the scan must skip it. */
#define GLFIX_MAX_LOADED 8
static char g_loaded_providers[GLFIX_MAX_LOADED][PATH_MAX];
static int g_loaded_provider_count;

static int glfix_collect_loaded_cb(struct dl_phdr_info *info, size_t size,
                                   void *data) {
  (void)size;
  (void)data;
  const char *name = info->dlpi_name;
  if (!name || !name[0])
    return 0;
  if (!strstr(name, "libEGL.so") && !strstr(name, "libGLES") &&
      !strstr(name, "mali") && !strstr(name, "Mali"))
    return 0;
  if (g_loaded_provider_count >= GLFIX_MAX_LOADED)
    return 0;
  if (realpath(name, g_loaded_providers[g_loaded_provider_count]))
    g_loaded_provider_count++;
  return 0;
}

static int glfix_candidate_is_loaded(const char *path) {
  char resolved[PATH_MAX];
  if (!realpath(path, resolved))
    return 0;
  for (int i = 0; i < g_loaded_provider_count; i++)
    if (strcmp(resolved, g_loaded_providers[i]) == 0)
      return 1;
  return 0;
}

/* A usable provider must export the GLES1 entry points this game calls and
 * its own EGL.  dlsym proves it instead of trusting the file name. */
static int glfix_blob_usable(const char *path) {
  void *handle = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
  if (!handle) {
    debugPrintf("glfix: %s rejected (dlopen: %s)\n", path, dlerror());
    return 0;
  }
  int ok = dlsym(handle, "glOrthof") != NULL &&
           dlsym(handle, "eglGetDisplay") != NULL;
  dlclose(handle);
  if (!ok)
    debugPrintf("glfix: %s rejected (no glOrthof/eglGetDisplay)\n", path);
  return ok;
}

/* Pre-context repair needs a harder proof: the resolved provider exported
 * every symbol and still refused the kernel driver (user/kernel API mismatch,
 * e.g. Mali user 10.6 vs kernel 11.7).  Only a real eglGetDisplay +
 * eglInitialize against the live kernel separates a blob that matches this
 * firmware from one that merely looks right.  A stale blob fails the same way
 * it failed the game; the matching one initializes.  The probe runs after
 * video teardown, in a process that is otherwise dead, so a misbehaving
 * candidate costs nothing that was not already lost. */
static int glfix_blob_initializes(const char *path) {
  if (!glfix_blob_usable(path))
    return 0;
  if (glfix_candidate_is_loaded(path)) {
    debugPrintf("glfix: %s skipped (already loaded and failing)\n", path);
    return 0;
  }
  void *handle = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
  if (!handle)
    return 0;
  void *(*get_display)(void *) =
      (void *(*)(void *))dlsym(handle, "eglGetDisplay");
  unsigned (*initialize)(void *, int *, int *) =
      (unsigned (*)(void *, int *, int *))dlsym(handle, "eglInitialize");
  unsigned (*terminate)(void *) =
      (unsigned (*)(void *))dlsym(handle, "eglTerminate");
  int ok = 0;
  if (get_display && initialize) {
    void *display = get_display(NULL); /* EGL_DEFAULT_DISPLAY */
    if (display) {
      int major = 0, minor = 0;
      if (initialize(display, &major, &minor)) {
        ok = 1;
        debugPrintf("glfix: %s initializes EGL %d.%d against this kernel\n",
                    path, major, minor);
        if (terminate)
          terminate(display);
      }
    }
  }
  dlclose(handle);
  if (!ok)
    debugPrintf("glfix: %s rejected (eglInitialize failed on this kernel)\n",
                path);
  return ok;
}

/* Deliberate pattern order: the -gbm variant matches SDL's KMSDRM backend and
 * must win over an alphabetical scan (which would find -dummy first). */
static const char *const glfix_patterns[] = {
    "libmali*-gbm.so*",  "libmali-bifrost-*.so*", "libmali-midgard-*.so*",
    "libmali.so*",       "libMali.so*",           "libGLES_mali.so*",
};
static const char *const glfix_dirs[] = {
    "/usr/lib/aarch64-linux-gnu", "/lib/aarch64-linux-gnu",
    "/usr/lib64", "/usr/lib", "/lib",
};

static int glfix_find_blob(char *out, size_t out_size,
                           const char *video_backend,
                           int (*probe)(const char *)) {
  const char *forced = getenv("SWORDIGO_GLFIX_BLOB");
  if (forced && *forced) {
    if (access(forced, R_OK) == 0 && probe(forced)) {
      snprintf(out, out_size, "%s", forced);
      return 1;
    }
    debugPrintf("glfix: forced blob unusable: %s\n", forced);
    return 0;
  }
  for (size_t p = 0; p < sizeof(glfix_patterns) / sizeof(glfix_patterns[0]);
       p++) {
    for (size_t d = 0; d < sizeof(glfix_dirs) / sizeof(glfix_dirs[0]); d++) {
      DIR *dir = opendir(glfix_dirs[d]);
      if (!dir)
        continue;
      struct dirent *entry;
      while ((entry = readdir(dir)) != NULL) {
        if (fnmatch(glfix_patterns[p], entry->d_name, 0) != 0)
          continue;
        if (!gl_provider_name_compatible(video_backend, entry->d_name)) {
          debugPrintf("glfix: %s/%s skipped (provider name incompatible "
                      "with backend '%s')\n", glfix_dirs[d], entry->d_name,
                      video_backend && video_backend[0] ? video_backend : "?");
          continue;
        }
        char candidate[1024];
        snprintf(candidate, sizeof(candidate), "%s/%s", glfix_dirs[d],
                 entry->d_name);
        if (probe(candidate)) {
          snprintf(out, out_size, "%s", candidate);
          closedir(dir);
          return 1;
        }
      }
      closedir(dir);
    }
  }
  return 0;
}

int glfix_renderer_is_broken(const char *renderer) {
  return renderer == NULL || renderer[0] == '\0';
}

/* One attempt ever: the marker survives the exec and stops any loop. */
static void glfix_reexec_with_preload(const char *blob) {
  const char *previous = getenv("LD_PRELOAD");
  char preload[2048];
  if (previous && *previous)
    snprintf(preload, sizeof(preload), "%s:%s", blob, previous);
  else
    snprintf(preload, sizeof(preload), "%s", blob);
  debugPrintf("glfix: re-executing with LD_PRELOAD=%s\n", preload);
  setenv("LD_PRELOAD", preload, 1);
  setenv(GLFIX_MARKER, "1", 1);
  if (g_saved_argv)
    execv("/proc/self/exe", g_saved_argv);
  debugPrintf("glfix: execv failed; continuing without repair\n");
}

static void glfix_reexec_with_sdl_provider_pair(const char *blob) {
  debugPrintf("glfix: re-executing with coherent SDL EGL/GLES provider=%s\n",
              blob);
  if (setenv("SDL_VIDEO_EGL_DRIVER", blob, 1) != 0 ||
      setenv("SDL_VIDEO_GL_DRIVER", blob, 1) != 0 ||
      setenv(GLFIX_MARKER, "1", 1) != 0) {
    debugPrintf("glfix: failed to publish coherent SDL provider pair\n");
    return;
  }
  if (g_saved_argv)
    execv("/proc/self/exe", g_saved_argv);
  debugPrintf("glfix: execv failed; continuing without repair\n");
}

/* Called once, right after the real context reported its renderer string.
 * Returns only when no repair applies; on repair it re-execs the process. */
void glfix_maybe_reexec(const char *renderer, const char *video_backend,
                        int window_opened, int context_current,
                        int drawable_positive, void (*teardown)(void)) {
  int coherent_enabled =
      glfix_enabled("adapter.gl-provider-coherent-sdl-reexec");
  int legacy_preload_enabled =
      glfix_enabled("adapter.gl-provider-reexec-preload");
  if (!coherent_enabled && !legacy_preload_enabled)
    return;
  if (!glfix_renderer_is_broken(renderer))
    return;
  if (getenv(GLFIX_MARKER)) {
    debugPrintf("glfix: renderer still empty after one provider repair; "
                "giving up\n");
    return;
  }
  char blob[1024];
  if (!glfix_find_blob(blob, sizeof(blob), video_backend,
                       glfix_blob_usable)) {
    debugPrintf("glfix: empty renderer but no usable Mali blob found\n");
    return;
  }
  if (coherent_enabled &&
      gl_provider_plan_sdl_pair(
          video_backend, renderer, blob, window_opened, context_current,
          drawable_positive, 1, 1) == GL_PROVIDER_SDL_PAIR_BIND_COHERENT) {
    debugPrintf("glfix: crossed SDL provider paths proven; binding one "
                "coherent EGL/GLES object\n");
    if (teardown)
      teardown();
    glfix_reexec_with_sdl_provider_pair(blob);
    return;
  }

  if (legacy_preload_enabled) {
    debugPrintf("glfix: empty renderer on the legacy crossed-SONAME stack; "
                "re-executing with the blob preloaded\n");
    if (teardown)
      teardown();
    glfix_reexec_with_preload(blob);
  }
}

/* GL init died before any window existed: the provider the linker resolved
 * refused the kernel driver.  Same repair, harder proof — every candidate
 * must initialize EGL against the live kernel before it earns the re-exec. */
void glfix_maybe_reexec_noctx(const char *reason, const char *video_backend,
                              void (*teardown)(void)) {
  if (!glfix_enabled("adapter.gl-provider-probe-init-reexec"))
    return;
  if (getenv(GLFIX_MARKER)) {
    debugPrintf("glfix: GL init still failing after preload; giving up\n");
    return;
  }
  debugPrintf("glfix: GL init failed pre-context (%s); "
              "probing alternate providers\n", reason ? reason : "unknown");
  char backend[64];
  snprintf(backend, sizeof(backend), "%s",
           video_backend && video_backend[0] ? video_backend : "");
  /* The display and the kernel driver fd must be free before the probe. */
  if (teardown)
    teardown();
  dl_iterate_phdr(glfix_collect_loaded_cb, NULL);
  char blob[1024];
  if (!glfix_find_blob(blob, sizeof(blob), backend,
                       glfix_blob_initializes)) {
    debugPrintf("glfix: no alternate provider initializes on this kernel; "
                "leaving the original failure\n");
    return;
  }
  glfix_reexec_with_preload(blob);
}
