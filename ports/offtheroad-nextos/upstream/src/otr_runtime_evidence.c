/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "otr_runtime_evidence.h"

#include "nxgl_graphics_contract.h"
#include "nxgl_graphics_contract_adapter.h"
#include "util.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_opengles2.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int g_contract_ok;
static int g_health_published;
static char g_game_dir[768];
static char g_input_receipt[1024];
static char g_input_load_receipt[1024];
static char g_audio_receipt[1024];

static int write_all(int fd, const char *data, size_t size) {
  size_t off = 0u;
  while (off < size) {
    ssize_t n = write(fd, data + off, size - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return -1;
    off += (size_t)n;
  }
  return 0;
}

static int write_atomic(const char *path, const char *data, size_t size) {
  char temp[1200];
  int fd;
  int n;
  if (path == NULL || data == NULL || strlen(path) > sizeof(temp) - 48u)
    return -1;
  n = snprintf(temp, sizeof(temp), "%s.tmp.%ld", path, (long)getpid());
  if (n < 0 || (size_t)n >= sizeof(temp)) return -1;
  (void)unlink(temp);
  fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) return -1;
  if (fchmod(fd, 0600) != 0 || write_all(fd, data, size) != 0 ||
      fsync(fd) != 0) {
    (void)close(fd);
    (void)unlink(temp);
    return -1;
  }
  if (close(fd) != 0 || rename(temp, path) != 0) {
    (void)unlink(temp);
    return -1;
  }
  return 0;
}

static const char *evidence_dir(void) {
  const char *proof = getenv("NXLAUNCH_PROOF_DIR");
  static char fallback[896];
  if (proof != NULL && proof[0] == '/') return proof;
  if (g_game_dir[0] == '\0') return NULL;
  (void)snprintf(fallback, sizeof(fallback), "%s/userdata", g_game_dir);
  return fallback;
}

const char *otr_input_evidence_path(void) { return g_input_receipt; }
const char *otr_input_load_evidence_path(void) { return g_input_load_receipt; }
const char *otr_audio_evidence_path(void) { return g_audio_receipt; }

static uint64_t identity_hash_add(uint64_t hash, const char *text) {
  const unsigned char *p = (const unsigned char *)(text ? text : "");
  while (*p != '\0') {
    hash ^= (uint64_t)*p++;
    hash *= UINT64_C(1099511628211);
  }
  hash ^= UINT64_C(0xff);
  return hash * UINT64_C(1099511628211);
}

static int bind_identity(const char *name, const char *value) {
  const char *current;
  if (name == NULL || value == NULL || value[0] == '\0') return 0;
  current = getenv(name);
  if (current != NULL && current[0] != '\0')
    return strcmp(current, value) == 0;
  return setenv(name, value, 0) == 0;
}

static int bridge_launcher_identity(void) {
  const char *generation = getenv("NXBOOTSTRAP_HEALTH_GENERATION");
  const char *port_id = getenv("NXBOOTSTRAP_HEALTH_PORT_ID");
  const char *run_id = getenv("NXBOOTSTRAP_HEALTH_RUN_ID");

  /* nxgl records NX_* provenance while nxbootstrap owns health identity.
   * Bind both namespaces and fail closed on a conflicting inherited value;
   * receipts must never describe a different generation/run/port. */
  if (generation != NULL && generation[0] != '\0' &&
      !bind_identity("NX_GENERATION", generation))
    return 0;
  if (run_id != NULL && run_id[0] != '\0' &&
      !bind_identity("NXOBS_RUN_ID", run_id))
    return 0;
  if (port_id != NULL && port_id[0] != '\0' &&
      !bind_identity("NX_PORT_ID", port_id))
    return 0;
  if (!bind_identity("NX_PORT_ID", "offtheroad") ||
      !bind_identity("NX_PORT_VERSION", "1.0.4"))
    return 0;
  return 1;
}

int otr_graphics_contract_start(const char *game_dir) {
  nxgl_graphics_contract contract;
  nxgl_graphics_evidence evidence;
  nxgl_graphics_reason reason;
  char json[16384];
  char line[4096];
  char path[1024];
  char cache_identity[96];
  const char *dir;
  size_t line_size;

  if (game_dir == NULL || strlen(game_dir) >= sizeof(g_game_dir)) return 0;
  (void)snprintf(g_game_dir, sizeof(g_game_dir), "%s", game_dir);
  if (!bridge_launcher_identity()) {
    fprintf(stderr, "[nxgl] conflicting launcher/runtime identity\n");
    return 0;
  }
  if (nxgl_graphics_contract_default(&contract) != 0) return 0;
  contract.api = NXGL_GRAPHICS_API_GLES;
  contract.profile = NXGL_GRAPHICS_PROFILE_ES;
  contract.version_major = 2;
  contract.version_minor = 0;
  contract.version_policy = NXGL_GRAPHICS_POLICY_MINIMUM;
  contract.shader_dialect = NXGL_SHADER_DIALECT_ESSL100;
  contract.drawable_ready_timeout_ms = 8000;

  nxgl_graphics_contract_adapter_set_resolver(
      (void *(*)(const char *))SDL_GL_GetProcAddress);
  reason = nxgl_graphics_contract_adapter_evidence(
      &contract, &evidence, json, sizeof(json));
  line_size = nxgl_graphics_contract_evidence_receipt(
      &contract, &evidence, line, sizeof(line));
  if (line_size > 0u) fprintf(stdout, "%s\n", line);

  dir = evidence_dir();
  if (dir != NULL) {
    (void)snprintf(path, sizeof(path), "%s/nx-graphics-evidence.json", dir);
    if (json[0] != '\0' && write_atomic(path, json, strlen(json)) != 0)
      fprintf(stderr, "[nxgl] nao foi possivel gravar %s\n", path);
    (void)snprintf(g_input_receipt, sizeof(g_input_receipt),
                   "%s/nx-input-evidence.json", dir);
    (void)snprintf(g_input_load_receipt, sizeof(g_input_load_receipt),
                   "%s/nxinput-gptk-load-evidence.json", dir);
    (void)snprintf(g_audio_receipt, sizeof(g_audio_receipt),
                   "%s/nx-audio-evidence.txt", dir);
  }
  g_contract_ok = reason == NXGL_GRAPHICS_OK &&
                  evidence.shader_probe == NXGL_SHADER_PROBE_PASS;
  if (g_contract_ok) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = identity_hash_add(hash, evidence.renderer);
    hash = identity_hash_add(hash, evidence.gl_version_str);
    hash = identity_hash_add(hash, evidence.glsl_version);
    hash = identity_hash_add(hash, evidence.dso_build_id);
    hash = identity_hash_add(hash, evidence.egl_build_id);
    (void)snprintf(cache_identity, sizeof(cache_identity),
                   "gles2-%016llx", (unsigned long long)hash);
    if (configure_shader_cache_identity(cache_identity) != 0) {
      fprintf(stderr, "[nxgl] shader cache namespace setup failed\n");
      g_contract_ok = 0;
    } else {
      fprintf(stdout,
              "[nxgl] disposable shader-cache namespace=%s owner-data=untouched\n",
              cache_identity);
    }
  }
  fprintf(stdout,
          "[nxgl] graphics-contract adapter=canonical phase=pre-engine "
          "verdict=%s\n",
          g_contract_ok ? "PASS" : "FAIL");
  fflush(stdout);
  return g_contract_ok;
}

static int identity_safe(const char *value, size_t max) {
  size_t i, n;
  if (value == NULL || value[0] == '\0') return 0;
  n = strlen(value);
  if (n > max) return 0;
  for (i = 0u; i < n; i++) {
    unsigned char c = (unsigned char)value[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
      return 0;
  }
  return 1;
}

static int generation_safe(const char *value) {
  size_t i;
  if (value == NULL || strlen(value) != 64u) return 0;
  for (i = 0u; i < 64u; i++)
    if (!((value[i] >= '0' && value[i] <= '9') ||
          (value[i] >= 'a' && value[i] <= 'f')))
      return 0;
  return 1;
}

/* Implements the nxbootstrap 0.6.37 health contract.  It intentionally does
 * nothing outside a supervised generation launch. */
static int publish_health(void) {
  const char *path = getenv("NXBOOTSTRAP_HEALTH_FILE");
  const char *schema = getenv("NXBOOTSTRAP_HEALTH_SCHEMA");
  const char *schema_version = getenv("NXBOOTSTRAP_HEALTH_SCHEMA_VERSION");
  const char *run_id = getenv("NXBOOTSTRAP_HEALTH_RUN_ID");
  const char *generation = getenv("NXBOOTSTRAP_HEALTH_GENERATION");
  const char *port_id = getenv("NXBOOTSTRAP_HEALTH_PORT_ID");
  const char *slash;
  const char *base;
  char parent[1024], expected[384], temp[512], receipt[768];
  struct stat dst, dir_st, temp_st, final_st;
  int dirfd = -1, fd = -1;
  int n;
  size_t parent_len;

  temp[0] = '\0';

  if (path == NULL || path[0] != '/' ||
      schema == NULL || strcmp(schema, "org.nextos.nxruntime.health") != 0 ||
      schema_version == NULL || strcmp(schema_version, "1") != 0 ||
      !identity_safe(run_id, 160u) || !generation_safe(generation) ||
      !identity_safe(port_id, 96u))
    return 0;
  slash = strrchr(path, '/');
  if (slash == NULL || slash == path || slash[1] == '\0') return 0;
  base = slash + 1;
  n = snprintf(expected, sizeof(expected), "health-%s-%s.json", port_id,
               run_id);
  if (n < 0 || (size_t)n >= sizeof(expected) || strcmp(base, expected) != 0)
    return 0;
  parent_len = (size_t)(slash - path);
  if (parent_len >= sizeof(parent)) return 0;
  memcpy(parent, path, parent_len);
  parent[parent_len] = '\0';
  dirfd = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (dirfd < 0 || fstat(dirfd, &dir_st) != 0 || !S_ISDIR(dir_st.st_mode) ||
      dir_st.st_uid != getuid() || (dir_st.st_mode & 0777) != 0700)
    goto fail;
  errno = 0;
  if (fstatat(dirfd, base, &dst, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT)
    goto fail;
  n = snprintf(temp, sizeof(temp), "%s.tmp.%ld", base, (long)getpid());
  if (n < 0 || (size_t)n >= sizeof(temp)) goto fail;
  n = snprintf(
      receipt, sizeof(receipt),
      "{\"schema\":\"org.nextos.nxruntime.health\",\"schema_version\":1,"
      "\"run_id\":\"%s\",\"generation\":\"%s\",\"port_id\":\"%s\","
      "\"status\":\"ready\"}\n",
      run_id, generation, port_id);
  if (n < 0 || (size_t)n >= sizeof(receipt)) goto fail;
  fd = openat(dirfd, temp,
              O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0 || fchmod(fd, 0600) != 0 || fstat(fd, &temp_st) != 0 ||
      !S_ISREG(temp_st.st_mode) || temp_st.st_uid != getuid() ||
      temp_st.st_nlink != 1 || (temp_st.st_mode & 0777) != 0600 ||
      write_all(fd, receipt, (size_t)n) != 0 || fsync(fd) != 0 ||
      close(fd) != 0)
    goto fail;
  fd = -1;
  if (renameat(dirfd, temp, dirfd, base) != 0 || fsync(dirfd) != 0 ||
      fstatat(dirfd, base, &final_st, AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISREG(final_st.st_mode) || final_st.st_uid != getuid() ||
      final_st.st_nlink != 1 || (final_st.st_mode & 0777) != 0600) {
    (void)unlinkat(dirfd, temp, 0);
    goto fail;
  }
  (void)close(dirfd);
  fprintf(stdout, "[health] generation=%s run_id=%s status=ready\n",
          generation, run_id);
  fflush(stdout);
  return 1;

fail:
  if (fd >= 0) (void)close(fd);
  if (dirfd >= 0) {
    if (temp[0] != '\0') (void)unlinkat(dirfd, temp, 0);
    (void)close(dirfd);
  }
  return 0;
}

int otr_graphics_pre_present(unsigned long frame, int width, int height) {
  enum { BLOCK = 8, GRID = 5 };
  unsigned char pixels[BLOCK * BLOCK * 4];
  GLint fbo = 0, viewport[4] = {0, 0, 0, 0};
  GLint scissor[4] = {0, 0, 0, 0};
  GLboolean mask[4] = {0, 0, 0, 0};
  GLboolean scissor_enabled;
  unsigned long coloured = 0u, alpha_zero = 0u, total = 0u;
  GLenum pre_error = GL_NO_ERROR, post_error = GL_NO_ERROR;
  int gx, gy;

  if (g_health_published) return 1;
  if (!g_contract_ok || width < BLOCK || height < BLOCK) return 0;
  if (frame != 120ul && frame != 600ul && (frame % 1800ul) != 0ul) return 0;

  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
  glGetIntegerv(GL_VIEWPORT, viewport);
  glGetIntegerv(GL_SCISSOR_BOX, scissor);
  glGetBooleanv(GL_COLOR_WRITEMASK, mask);
  scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
  for (int guard = 0; guard < 8; guard++) {
    GLenum err = glGetError();
    if (err == GL_NO_ERROR) break;
    pre_error = err;
  }
  for (gy = 0; gy < GRID; gy++) {
    for (gx = 0; gx < GRID; gx++) {
      int x = ((gx + 1) * width) / (GRID + 1) - BLOCK / 2;
      int y = ((gy + 1) * height) / (GRID + 1) - BLOCK / 2;
      size_t i;
      glReadPixels(x, y, BLOCK, BLOCK, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
      for (i = 0u; i < (size_t)(BLOCK * BLOCK); i++) {
        const unsigned char *p = pixels + i * 4u;
        if (p[0] > 2u || p[1] > 2u || p[2] > 2u) coloured++;
        if (p[3] == 0u) alpha_zero++;
        total++;
      }
    }
  }
  post_error = glGetError();
  fprintf(stdout,
          "[gfx-proof] frame=%lu sample=pre-overlay/pre-present fbo=%d "
          "viewport=%d,%d,%d,%d scissor=%d:%d,%d,%d,%d "
          "mask=%d%d%d%d rgb_non_black=%lu/%lu alpha0=%lu/%lu "
          "pre_error=0x%x post_error=0x%x\n",
          frame, fbo, viewport[0], viewport[1], viewport[2], viewport[3],
          scissor_enabled ? 1 : 0, scissor[0], scissor[1], scissor[2],
          scissor[3], mask[0] ? 1 : 0, mask[1] ? 1 : 0, mask[2] ? 1 : 0,
          mask[3] ? 1 : 0, coloured, total, alpha_zero, total,
          (unsigned)pre_error, (unsigned)post_error);
  fflush(stdout);

  /* Never let the cursor overlay, alpha alone, a non-default FBO, or a failed
   * readback promote health.  RGB from the game's presentable framebuffer is
   * mandatory. */
  if (fbo == 0 && viewport[2] > 1 && viewport[3] > 1 &&
      post_error == GL_NO_ERROR && coloured >= 4u) {
    g_health_published = publish_health();
  }
  return g_health_published;
}
