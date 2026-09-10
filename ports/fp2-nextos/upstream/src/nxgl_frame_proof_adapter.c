/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* RTLD_DEFAULT */
#endif
#include "nxgl_frame_proof_adapter.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdint.h>
#include <limits.h>

/* Deliberately no SDL dependency: the adapter reads the framebuffer the port
 * already made current, so it works the same for an SDL port and a raw-EGL one.
 */
#include <dlfcn.h>

/* GL is resolved at runtime rather than linked. A so-loader port routes every
 * gl* call through its own shim and does not link a GLES library at all, so a
 * direct call here fails to link; a port that does link one still resolves the
 * same symbols through dlsym. The port may install its own resolver when its
 * shim is the only place the real entry points exist. */
#define NXGL_FP_RGBA 0x1908
#define NXGL_FP_UNSIGNED_BYTE 0x1401
#define NXGL_FP_VIEWPORT 0x0BA2
#define NXGL_FP_RENDERER 0x1F01
#define NXGL_FP_VERSION 0x1F02
#define NXGL_FP_EXTENSIONS 0x1F03
#define NXGL_FP_PACK_ALIGNMENT 0x0D05
#define NXGL_FP_PACK_ROW_LENGTH 0x0D02
#define NXGL_FP_PACK_SKIP_ROWS 0x0D03
#define NXGL_FP_PACK_SKIP_PIXELS 0x0D04
#define NXGL_FP_FRAMEBUFFER_BINDING 0x8CA6
#define NXGL_FP_READ_FRAMEBUFFER_BINDING 0x8CAA
#define NXGL_FP_PIXEL_PACK_BUFFER_BINDING 0x88ED

#define NXGL_FP_MAX_DIMENSION 16384
#define NXGL_FP_MAX_READBACK_BYTES ((size_t)64 * 1024 * 1024)
#define NXGL_FP_FATAL_STREAK 3

typedef void (*nxgl_fp_read_pixels)(int, int, int, int, unsigned, unsigned,
                                    void *);
typedef void (*nxgl_fp_get_integerv)(unsigned, int *);
typedef const unsigned char *(*nxgl_fp_get_string)(unsigned);

static void *(*g_resolver)(const char *);

/* The adapter is a render-thread API, but ports occasionally emit shutdown
 * receipts from another thread. A non-blocking C99 guard serializes all state:
 * concurrent/reentrant calls fail closed instead of spinning while a GL
 * callback is active (which could deadlock the render thread). */
static volatile int g_adapter_lock;

static int adapter_enter(void) {
  if (__sync_lock_test_and_set(&g_adapter_lock, 1) == 0)
    return 1;
  fprintf(stderr, "gl: frame proof refused (concurrent/reentrant call)\n");
  fflush(stderr);
  return 0;
}

static void adapter_unlock(void) {
  __sync_lock_release(&g_adapter_lock);
}

void nxgl_frame_proof_set_resolver(void *(*resolver)(const char *)) {
  if (!adapter_enter())
    return;
  g_resolver = resolver;
  adapter_unlock();
}

static void *resolve_gl(const char *name) {
  if (g_resolver) {
    void *found = g_resolver(name);
    if (found)
      return found;
  }
  void *found = dlsym(RTLD_DEFAULT, name);
  if (found)
    return found;
  /* An SDL port resolves GL through SDL_GL_GetProcAddress, and on some images
   * that is the only path to the driver's real entry points -- glReadPixels is
   * not a global symbol on the Mali-450 image. Reach it through dlsym so this
   * adapter still links into a port that does not include SDL headers. */
  void *(*sdl_get_proc)(const char *) =
      (void *(*)(const char *))dlsym(RTLD_DEFAULT, "SDL_GL_GetProcAddress");
  if (sdl_get_proc)
    return sdl_get_proc(name);
  return NULL;
}

/* Mirrors nxgl_classify_frame_proof_v2 / nxgl_classify_launch_context_v2. The
 * policy lives in nxgl and stays pure; this adapter is the half that has to
 * touch GL, so it is vendored into the port rather than linked into nxgl. */
#define FRAME_PROOF_MIN_NON_BLACK 0.5 /* percent of pixels */

#define VIDEO_PROOF_SCHEMA "org.nextos.nxruntime.video-proof"
#define VIDEO_PROOF_SCHEMA_VERSION 1
#define VIDEO_PROOF_PATH_MAX 1024
#define VIDEO_PROOF_TOKEN_MAX 192

/* Machine-readable runtime contract consumed by nxbootstrap. This receipt is
 * deliberately separate from the generation health receipt: audio, a live
 * PID, exit 0 and even a created GL context do not prove that a frame was
 * drawn. A fatal receipt may replace an earlier OK receipt; once fatal, it is
 * irreversible for the process. */
enum video_proof_state {
  VIDEO_PROOF_NONE = 0,
  VIDEO_PROOF_OK,
  VIDEO_PROOF_FATAL_PENDING,
  VIDEO_PROOF_FATAL_PUBLISHED
};

static enum video_proof_state g_video_proof_state;
static const char *g_fatal_verdict;
static const char *g_fatal_reason;
static int g_fatal_active;
static int g_fatal_close_pending;
static int g_video_contract_captured;
static char g_video_path[VIDEO_PROOF_PATH_MAX];
static char g_video_run_id[VIDEO_PROOF_TOKEN_MAX + 1];
static char g_video_generation[VIDEO_PROOF_TOKEN_MAX + 1];
static char g_video_port_id[VIDEO_PROOF_TOKEN_MAX + 1];
static int g_health_contract_captured;
static char g_health_path[VIDEO_PROOF_PATH_MAX];

static int video_proof_token_valid(const char *value) {
  size_t i;

  if (value == NULL || value[0] == '\0')
    return 0;
  for (i = 0; value[i] != '\0'; i++) {
    unsigned char c = (unsigned char)value[i];
    if (i >= VIDEO_PROOF_TOKEN_MAX ||
        (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
         !(c >= '0' && c <= '9') && c != '.' && c != '_' && c != '-'))
      return 0;
  }
  return 1;
}

static int video_proof_path_valid(const char *path) {
  size_t len;

  if (path == NULL || path[0] != '/')
    return 0;
  len = strlen(path);
  if (len < 2 || len >= VIDEO_PROOF_PATH_MAX || path[len - 1] == '/')
    return 0;
  if (strchr(path, '\n') != NULL || strchr(path, '\r') != NULL ||
      strstr(path, "//") != NULL || strstr(path, "/../") != NULL ||
      strstr(path, "/./") != NULL)
    return 0;
  return !(len >= 3 && strcmp(path + len - 3, "/..") == 0) &&
         !(len >= 2 && strcmp(path + len - 2, "/.") == 0);
}

static int video_proof_parent_private(const char *path) {
  char parent[VIDEO_PROOF_PATH_MAX];
  char *slash;
  struct stat st;
  size_t len = strlen(path);

  if (len >= sizeof parent)
    return 0;
  memcpy(parent, path, len + 1);
  slash = strrchr(parent, '/');
  if (slash == NULL || slash == parent)
    return 0;
  *slash = '\0';
  if (lstat(parent, &st) != 0 || !S_ISDIR(st.st_mode) ||
      st.st_uid != geteuid() || (st.st_mode & 0077) != 0)
    return 0;
  return 1;
}

static int video_proof_existing_safe(const char *path) {
  struct stat st;

  if (lstat(path, &st) != 0)
    return errno == ENOENT;
  return S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
         (st.st_mode & 0777) == 0600 && st.st_nlink == 1;
}

static int video_proof_write_all(int fd, const void *data, size_t len) {
  const unsigned char *bytes = (const unsigned char *)data;
  size_t done = 0;

  while (done < len) {
    ssize_t wrote = write(fd, bytes + done, len - done);
    if (wrote > 0) {
      done += (size_t)wrote;
      continue;
    }
    if (wrote < 0 && errno == EINTR)
      continue;
    return 0;
  }
  return 1;
}

static int video_proof_parent_path(const char *path, char *parent,
                                   size_t parent_cap) {
  char *slash;
  size_t len;

  if (path == NULL || parent == NULL || parent_cap == 0)
    return 0;
  len = strlen(path);
  if (len + 1 > parent_cap)
    return 0;
  memcpy(parent, path, len + 1);
  slash = strrchr(parent, '/');
  if (slash == NULL || slash == parent)
    return 0;
  *slash = '\0';
  return 1;
}

static int video_proof_fsync_parent(const char *path) {
  char parent[VIDEO_PROOF_PATH_MAX];
  int fd;
  int ok;

  if (!video_proof_parent_path(path, parent, sizeof parent))
    return 0;
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
  fd = open(parent, O_RDONLY | O_DIRECTORY);
  if (fd < 0)
    return 0;
  ok = fsync(fd) == 0;
  if (close(fd) != 0)
    ok = 0;
  return ok;
}

/* If a fatal observation follows an already published OK but the fatal
 * replacement cannot be written, leaving the old JSON intact would silently
 * authorize the broken run.  Emptying the already validated inode makes the
 * receipt unparsable while retaining a retryable, safe destination. */
static int video_proof_revoke_existing(const char *path) {
  struct stat st;
  int fd;
  int ok;

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
  if (!video_proof_path_valid(path) || !video_proof_parent_private(path) ||
      lstat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
      st.st_uid != geteuid() || (st.st_mode & 0777) != 0600 ||
      st.st_nlink != 1)
    return 0;
  fd = open(path, O_WRONLY | O_NOFOLLOW);
  if (fd < 0)
    return 0;
  ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
       st.st_uid == geteuid() && (st.st_mode & 0777) == 0600 &&
       st.st_nlink == 1 && ftruncate(fd, 0) == 0 && fsync(fd) == 0;
  if (close(fd) != 0)
    ok = 0;
  if (ok)
    (void)video_proof_fsync_parent(path);
  return ok;
}

static void capture_health_contract_locked(void) {
  const char *path;
  size_t len;

  if (g_health_contract_captured != 0)
    return;
  path = getenv("NXBOOTSTRAP_HEALTH_FILE");
  if (path == NULL || path[0] == '\0') {
    g_health_path[0] = '\0';
    g_health_contract_captured = 1;
    return;
  }
  len = strlen(path);
  if (!video_proof_path_valid(path) || len + 1u > sizeof g_health_path) {
    g_health_contract_captured = -1;
    return;
  }
  memcpy(g_health_path, path, len + 1u);
  g_health_contract_captured = 1;
}

static int revoke_health_receipt_locked(void) {
  struct stat st;

  capture_health_contract_locked();
  if (g_health_contract_captured < 0)
    return 0;
  if (g_health_path[0] == '\0')
    return 1;
  if (lstat(g_health_path, &st) != 0)
    return errno == ENOENT;
  if (!video_proof_parent_private(g_health_path) || !S_ISREG(st.st_mode) ||
      st.st_uid != geteuid() || (st.st_mode & 0777) != 0600 ||
      st.st_nlink != 1)
    return 0;
  if (unlink(g_health_path) != 0)
    return 0;
  return video_proof_fsync_parent(g_health_path);
}

static int publish_video_proof_file(const char *verdict,
                                    const char *reason) {
  const char *path;
  const char *run_id;
  const char *generation;
  const char *port_id;
  char json[1024], temp[VIDEO_PROOF_PATH_MAX + 64];
  struct stat st;
  int fd = -1, fatal, json_len, temp_len;
  int temp_created = 0, renamed = 0;
  static unsigned long serial;

  fatal = strcmp(verdict, "OK") != 0;
  if (fatal) {
    if (g_video_proof_state == VIDEO_PROOF_FATAL_PUBLISHED)
      return 1;
    g_video_proof_state = VIDEO_PROOF_FATAL_PENDING;
    g_fatal_verdict = verdict;
    g_fatal_reason = reason;
  } else {
    if (g_video_proof_state == VIDEO_PROOF_OK)
      return 1;
    if (g_video_proof_state == VIDEO_PROOF_FATAL_PENDING ||
        g_video_proof_state == VIDEO_PROOF_FATAL_PUBLISHED)
      return 0;
  }

  if (g_video_contract_captured) {
    path = g_video_path;
    run_id = g_video_run_id;
    generation = g_video_generation;
    port_id = g_video_port_id;
  } else {
    path = getenv("NXBOOTSTRAP_VIDEO_FILE");
    run_id = getenv("NXBOOTSTRAP_HEALTH_RUN_ID");
    generation = getenv("NXBOOTSTRAP_HEALTH_GENERATION");
    port_id = getenv("NXBOOTSTRAP_HEALTH_PORT_ID");
  }

  if (path == NULL || path[0] == '\0') {
    if (!fatal)
      g_video_proof_state = VIDEO_PROOF_OK;
    return !fatal;
  }
  if ((strcmp(verdict, "OK") != 0 && strcmp(verdict, "BLACK") != 0 &&
       strcmp(verdict, "DEAD-CONTEXT") != 0) ||
      (strcmp(reason, "non-black") != 0 &&
       strcmp(reason, "black-streak") != 0 &&
       strcmp(reason, "all-black") != 0 &&
       strcmp(reason, "dead-context") != 0) ||
      !video_proof_path_valid(path) ||
      !video_proof_token_valid(run_id) ||
      !video_proof_token_valid(generation) ||
      !video_proof_token_valid(port_id)) {
    fprintf(stderr, "VIDEO-PROOF: receipt refused (unsafe contract)\n");
    fflush(stderr);
    if (fatal && video_proof_existing_safe(path) &&
        video_proof_revoke_existing(path)) {
      fprintf(stderr, "VIDEO-PROOF: prior OK revoked; fatal retry pending\n");
      fflush(stderr);
    }
    return 0;
  }
  if (!g_video_contract_captured) {
    memcpy(g_video_path, path, strlen(path) + 1u);
    memcpy(g_video_run_id, run_id, strlen(run_id) + 1u);
    memcpy(g_video_generation, generation, strlen(generation) + 1u);
    memcpy(g_video_port_id, port_id, strlen(port_id) + 1u);
    g_video_contract_captured = 1;
    path = g_video_path;
    run_id = g_video_run_id;
    generation = g_video_generation;
    port_id = g_video_port_id;
  }
  if (!video_proof_parent_private(path) ||
      !video_proof_existing_safe(path)) {
    fprintf(stderr, "VIDEO-PROOF: receipt refused (unsafe contract)\n");
    fflush(stderr);
    if (fatal && video_proof_revoke_existing(path)) {
      fprintf(stderr, "VIDEO-PROOF: prior OK revoked; fatal retry pending\n");
      fflush(stderr);
    }
    return 0;
  }

  json_len = snprintf(json, sizeof json,
                      "{\"schema\":\"%s\",\"schema_version\":%d,"
                      "\"run_id\":\"%s\",\"generation\":\"%s\","
                      "\"port_id\":\"%s\",\"verdict\":\"%s\","
                      "\"reason\":\"%s\"}\n",
                      VIDEO_PROOF_SCHEMA, VIDEO_PROOF_SCHEMA_VERSION, run_id,
                      generation, port_id, verdict, reason);
  if (json_len <= 0 || (size_t)json_len >= sizeof json)
    goto failed;
  temp_len = snprintf(temp, sizeof temp, "%s.tmp.%ld.%lu", path,
                      (long)getpid(), ++serial);
  if (temp_len <= 0 || (size_t)temp_len >= sizeof temp)
    goto failed;

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
  fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if (fd < 0)
    goto failed;
  temp_created = 1;
  {
    int write_ok = fchmod(fd, 0600) == 0 && fstat(fd, &st) == 0 &&
                   S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
                   (st.st_mode & 0777) == 0600 && st.st_nlink == 1 &&
                   video_proof_write_all(fd, json, (size_t)json_len) &&
                   fsync(fd) == 0;
    if (close(fd) != 0)
      write_ok = 0;
    fd = -1;
    if (!write_ok)
      goto failed;
  }
  if (!video_proof_existing_safe(path) || rename(temp, path) != 0)
    goto failed;
  temp_created = 0;
  renamed = 1;
  if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
      st.st_uid != geteuid() || (st.st_mode & 0777) != 0600 ||
      st.st_nlink != 1 || !video_proof_fsync_parent(path))
    goto failed;

  g_video_proof_state = fatal ? VIDEO_PROOF_FATAL_PUBLISHED : VIDEO_PROOF_OK;
  printf("VIDEO-PROOF: verdict=%s reason=%s receipt=published\n", verdict,
         reason);
  fflush(stdout);
  return 1;

failed:
  if (fd >= 0)
    close(fd);
  /* Never unlink a guessed pathname whose O_EXCL open failed: it may be an
   * unrelated file deliberately occupying the name. */
  if (temp_created)
    (void)unlink(temp);
  if (!renamed && video_proof_existing_safe(path) &&
      video_proof_revoke_existing(path)) {
    fprintf(stderr, "VIDEO-PROOF: previous receipt revoked; retry pending\n");
    fflush(stderr);
  } else if (!fatal && renamed) {
    /* An OK that did not complete the durability/verification boundary is not
     * authority. Make it unparsable before allowing another attempt. */
    (void)video_proof_revoke_existing(path);
  }
  fprintf(stderr, "VIDEO-PROOF: receipt write failed\n");
  fflush(stderr);
  return 0;
}


/* Write the exact first visibly non-black sample as RGBA PNG when requested.
 * Stored deflate blocks need no zlib and do not change the port's link line. */
static uint32_t crc_table[256];
static void crc_init(void) {
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++)
      c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crc_table[n] = c;
  }
}
static uint32_t crc_update(uint32_t crc, const unsigned char *buf, size_t len) {
  for (size_t i = 0; i < len; i++)
    crc = crc_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
  return crc;
}
static uint32_t adler32(const unsigned char *buf, size_t len) {
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < len; i++) { a = (a + buf[i]) % 65521u; b = (b + a) % 65521u; }
  return (b << 16) | a;
}
static void be32(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
  p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}
static int png_chunk(int fd, const char *type, const unsigned char *data,
                     size_t len) {
  unsigned char hdr[8];
  uint32_t crc;
  unsigned char c[4];

  if (len > UINT32_MAX)
    return 0;
  be32(hdr, (uint32_t)len); memcpy(hdr + 4, type, 4);
  crc = crc_update(0xFFFFFFFFu, (const unsigned char *)type, 4);
  if (len)
    crc = crc_update(crc, data, len);
  be32(c, crc ^ 0xFFFFFFFFu);
  return video_proof_write_all(fd, hdr, sizeof hdr) &&
         (!len || video_proof_write_all(fd, data, len)) &&
         video_proof_write_all(fd, c, sizeof c);
}

static int proof_directory_private(const char *dir) {
  struct stat st;
  size_t len;

  if (dir == NULL || dir[0] != '/')
    return 0;
  len = strlen(dir);
  if (len < 2 || len >= VIDEO_PROOF_PATH_MAX || dir[len - 1] == '/' ||
      strchr(dir, '\n') != NULL || strchr(dir, '\r') != NULL ||
      strstr(dir, "//") != NULL || strstr(dir, "/../") != NULL ||
      strstr(dir, "/./") != NULL)
    return 0;
  return lstat(dir, &st) == 0 && S_ISDIR(st.st_mode) &&
         st.st_uid == geteuid() && (st.st_mode & 0077) == 0;
}

static int write_frame_png(const char *dir, int width, int height,
                           size_t source_stride,
                           const unsigned char *rgba) {
  size_t row, raw_len, blocks, zlen, o;
  unsigned char *raw = NULL, *z = NULL;
  char path[VIDEO_PROOF_PATH_MAX];
  char temp[VIDEO_PROOF_PATH_MAX + 64];
  static unsigned long serial;
  int path_len, temp_len, fd = -1;
  int temp_created = 0, renamed = 0, ok = 0;
  struct stat st;
  static const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  unsigned char ihdr[13];

  if (!proof_directory_private(dir) || width <= 0 || height <= 0 ||
      rgba == NULL || (size_t)width > (SIZE_MAX - 1u) / 4u)
    goto done;
  row = 1u + (size_t)width * 4u;
  if ((size_t)height > SIZE_MAX / row)
    goto done;
  raw_len = row * (size_t)height;
  if (raw_len == 0 || raw_len > NXGL_FP_MAX_READBACK_BYTES +
                                      (size_t)NXGL_FP_MAX_DIMENSION)
    goto done;
  blocks = (raw_len + 65534u) / 65535u;
  if (blocks > (SIZE_MAX - raw_len - 6u) / 5u)
    goto done;
  zlen = 2u + raw_len + blocks * 5u + 4u;
  raw = (unsigned char *)malloc(raw_len);
  z = (unsigned char *)malloc(zlen);
  if (raw == NULL || z == NULL)
    goto done;
  for (int y = 0; y < height; y++) {
    unsigned char *dst = raw + (size_t)y * row;
    const unsigned char *src =
        rgba + (size_t)(height - 1 - y) * source_stride;
    dst[0] = 0;
    memcpy(dst + 1, src, (size_t)width * 4u);
  }
  /* zlib stream of stored blocks (max 65535 bytes each). */
  o = 0; z[o++] = 0x78; z[o++] = 0x01;
  for (size_t off = 0; off < raw_len; off += 65535) {
    size_t n = raw_len - off; if (n > 65535) n = 65535;
    z[o++] = (off + n == raw_len) ? 1 : 0;
    z[o++] = (unsigned char)(n & 0xFF); z[o++] = (unsigned char)(n >> 8);
    z[o++] = (unsigned char)(~n & 0xFF); z[o++] = (unsigned char)((~n >> 8) & 0xFF);
    memcpy(z + o, raw + off, n); o += n;
  }
  be32(z + o, adler32(raw, raw_len)); o += 4;
  if (o != zlen)
    goto done;

  path_len = snprintf(path, sizeof path, "%s/frame-proof.png", dir);
  if (path_len <= 0 || (size_t)path_len >= sizeof path ||
      lstat(path, &st) == 0 || errno != ENOENT)
    goto done;
  temp_len = snprintf(temp, sizeof temp, "%s.tmp.%ld.%lu", path,
                      (long)getpid(), ++serial);
  if (temp_len <= 0 || (size_t)temp_len >= sizeof temp)
    goto done;
  fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if (fd < 0)
    goto done;
  temp_created = 1;
  be32(ihdr, (uint32_t)width); be32(ihdr + 4, (uint32_t)height);
  ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  crc_init();
  if (fchmod(fd, 0600) != 0 || fstat(fd, &st) != 0 ||
      !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
      (st.st_mode & 0777) != 0600 || st.st_nlink != 1 ||
      !video_proof_write_all(fd, sig, sizeof sig) ||
      !png_chunk(fd, "IHDR", ihdr, sizeof ihdr) ||
      !png_chunk(fd, "IDAT", z, o) || !png_chunk(fd, "IEND", NULL, 0) ||
      fsync(fd) != 0)
    goto done;
  if (close(fd) != 0) {
    fd = -1;
    goto done;
  }
  fd = -1;
  if (rename(temp, path) != 0)
    goto done;
  temp_created = 0;
  renamed = 1;
  if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
      st.st_uid != geteuid() || (st.st_mode & 0777) != 0600 ||
      st.st_nlink != 1 || !video_proof_fsync_parent(path))
    goto done;
  ok = 1;

done:
  if (fd >= 0)
    (void)close(fd);
  if (temp_created)
    (void)unlink(temp);
  if (!ok && renamed)
    (void)unlink(path);
  free(z);
  free(raw);
  if (ok) {
    printf("gl: frame proof image written %s (%dx%d RGBA)\n", path, width,
           height);
    fflush(stdout);
  } else {
    fprintf(stderr, "gl: frame proof image write failed\n");
    fflush(stderr);
  }
  return ok;
}

static double g_best_non_black = -1.0;
static double g_best_presented_non_black = -1.0;
static int g_dead_context; /* GL_RENDERER nulo em alguma amostra */
static int g_samples;
static int g_unmeasured; /* probes that could not read the frame at all */
static int g_published;
static const char *g_last_unmeasured_reason = "readback-unresolved";
static int g_png_evidence_ready;
static int g_png_required_failed;

/* E2: context + evidence for the single-line "VIDEO:" receipt. All additive;
 * a port that never registers context still gets the receipt with "?". */
static int g_ctx_w, g_ctx_h, g_ctx_set;
static char g_ctx_driver[64], g_ctx_renderer[96], g_ctx_version[96];
static int g_invalid_drawable;     /* probes refused for a 0x0/absurd drawable */
static double g_last_alpha0_pct;   /* alpha==0 share of the last probe */
static double g_last_rgb_non_black;/* RGB non-black even when alpha is zero */
static unsigned long g_frames_presented;
static int g_black_streak;
static int g_dead_streak;
static int g_streak_screamed;
static int g_auto_proof_state;     /* 0=unread, 1=on, 2=off */

/* V3: where each counted sample was taken. After the swap the backbuffer is
 * undefined on tile-based GPUs, so the receipt must say where it read. */
static int g_point_before;        /* samples taken before-present */
static int g_point_after;         /* samples taken after-present */
static int g_point_unspecified;   /* legacy nxgl_frame_proof_sample() calls */
static int g_pending_point = -1;  /* -1 unspecified; else the enum value */

static void copy_bounded(char *dst, size_t cap, const char *src) {
  size_t i = 0;
  if (src)
    for (; src[i] && i + 1 < cap; i++)
      dst[i] = (src[i] == '\n' || src[i] == '"') ? ' ' : src[i];
  dst[i] = 0;
}

void nxgl_frame_proof_set_video_context(int width, int height,
                                        const char *driver,
                                        const char *renderer,
                                        const char *version) {
  if (!adapter_enter())
    return;
  g_ctx_w = width;
  g_ctx_h = height;
  copy_bounded(g_ctx_driver, sizeof g_ctx_driver, driver);
  copy_bounded(g_ctx_renderer, sizeof g_ctx_renderer, renderer);
  copy_bounded(g_ctx_version, sizeof g_ctx_version, version);
  g_ctx_set = 1;
  adapter_unlock();
}

/* A launch that could never put an image on the panel cannot be used to accuse
 * the port of drawing nothing. */
static const char *launch_context(int *conclusive) {
  const char *frontend = getenv("NXLAUNCH_FRONTEND");

  /* Mirror nxgl_classify_launch_context_v2 literally: a valid explicit
   * frontend launch wins even if an operator has a parallel SSH login. The
   * only valid affirmative environment encoding is exactly "1"; "0" is not
   * a frontend attestation. */
  if (frontend != NULL && strcmp(frontend, "1") == 0) {
    *conclusive = 1;
    return "frontend";
  }
  if (getenv("SSH_CONNECTION") || getenv("SSH_TTY") || getenv("SSH_CLIENT")) {
    *conclusive = 0;
    return "remote";
  }
  {
    const char *tty = ttyname(0);
    if (tty && strncmp(tty, "/dev/tty", 8) == 0 && tty[8] >= '0' &&
        tty[8] <= '9') {
      *conclusive = 1;
      return "console";
    }
  }
  *conclusive = 0;
  return "unknown";
}

void nxgl_frame_proof_launch_receipt(void) {
  int conclusive = 0;
  const char *context = launch_context(&conclusive);
  if (!adapter_enter())
    return;
  capture_health_contract_locked();
  printf("launch: context=%s can-prove-image=%s\n", context,
         conclusive ? "yes" : "no");
  fflush(stdout);
  adapter_unlock();
}

typedef struct frame_read_layout {
  size_t row_bytes;
  size_t stride;
  size_t total_bytes;
} frame_read_layout;

static int extension_named(const char *extensions, const char *needle) {
  const char *found;
  size_t needle_len;

  if (extensions == NULL || needle == NULL || needle[0] == '\0')
    return 0;
  needle_len = strlen(needle);
  found = extensions;
  while ((found = strstr(found, needle)) != NULL) {
    if ((found == extensions || found[-1] == ' ') &&
        (found[needle_len] == '\0' || found[needle_len] == ' '))
      return 1;
    found += needle_len;
  }
  return 0;
}

static int gles_major_from_version(const char *version) {
  const char *cursor;

  if (version == NULL || strncmp(version, "OpenGL ES", 9) != 0)
    return 0;
  cursor = version + 9;
  while (*cursor != '\0' && (*cursor < '0' || *cursor > '9'))
    cursor++;
  if (*cursor < '1' || *cursor > '9')
    return 0;
  return *cursor - '0';
}

static int query_integer(nxgl_fp_get_integerv get_integerv, unsigned name,
                         int *value) {
  int measured = INT_MIN;
  get_integerv(name, &measured);
  if (measured == INT_MIN)
    return 0;
  *value = measured;
  return 1;
}

static int frame_read_preflight(int width, int height,
                                frame_read_layout *layout) {
  nxgl_fp_get_string get_string =
      (nxgl_fp_get_string)resolve_gl("glGetString");
  nxgl_fp_get_integerv get_integerv =
      (nxgl_fp_get_integerv)resolve_gl("glGetIntegerv");
  const unsigned char *renderer;
  const unsigned char *version_value;
  const unsigned char *extensions_value;
  const char *extensions;
  int major, alignment, binding;
  int pack_row_length = 0, pack_skip_rows = 0, pack_skip_pixels = 0;
  size_t row_bytes, stride, total;

  if (get_string == NULL || get_integerv == NULL) {
    g_last_unmeasured_reason = "gl-state-unresolved";
    return 0;
  }
  renderer = get_string(NXGL_FP_RENDERER);
  if (renderer == NULL || renderer[0] == '\0') {
    g_dead_context++;
    g_last_unmeasured_reason = "renderer-null-driverless-provider";
    printf("gl: frame probe DEAD-CONTEXT (GL_RENDERER nulo); leitura de "
           "pixels deste contexto NAO vale como prova\n");
    fflush(stdout);
    return -1;
  }
  version_value = get_string(NXGL_FP_VERSION);
  major = gles_major_from_version((const char *)version_value);
  if (major == 0) {
    g_last_unmeasured_reason = version_value == NULL
                                   ? "version-unavailable"
                                   : "non-gles-context";
    return 0;
  }
  extensions_value = get_string(NXGL_FP_EXTENSIONS);
  if (extensions_value == NULL) {
    g_last_unmeasured_reason = "extensions-unavailable";
    return 0;
  }
  extensions = (const char *)extensions_value;

  if (!query_integer(get_integerv, NXGL_FP_PACK_ALIGNMENT, &alignment) ||
      (alignment != 1 && alignment != 2 && alignment != 4 && alignment != 8)) {
    g_last_unmeasured_reason = "pack-alignment-unproven";
    return 0;
  }

  /* Prove that glReadPixels observes the default/presentable framebuffer.
   * ES1 has no alternate framebuffer unless OES_framebuffer_object exists. */
  if (major >= 3) {
    if (!query_integer(get_integerv, NXGL_FP_FRAMEBUFFER_BINDING, &binding) ||
        binding != 0 ||
        !query_integer(get_integerv, NXGL_FP_READ_FRAMEBUFFER_BINDING,
                       &binding) || binding != 0) {
      g_last_unmeasured_reason = "non-default-framebuffer";
      return 0;
    }
  } else if (major >= 2 ||
             extension_named(extensions, "GL_OES_framebuffer_object")) {
    if (!query_integer(get_integerv, NXGL_FP_FRAMEBUFFER_BINDING, &binding) ||
        binding != 0) {
      g_last_unmeasured_reason = "non-default-framebuffer";
      return 0;
    }
  }

  if (major >= 3 ||
      extension_named(extensions, "GL_NV_pixel_buffer_object") ||
      extension_named(extensions, "GL_EXT_pixel_buffer_object")) {
    if (!query_integer(get_integerv, NXGL_FP_PIXEL_PACK_BUFFER_BINDING,
                       &binding) || binding != 0) {
      g_last_unmeasured_reason = "pixel-pack-buffer-bound";
      return 0;
    }
  }
  if (major >= 3 || extension_named(extensions, "GL_NV_pack_subimage")) {
    if (!query_integer(get_integerv, NXGL_FP_PACK_ROW_LENGTH,
                       &pack_row_length) ||
        !query_integer(get_integerv, NXGL_FP_PACK_SKIP_ROWS,
                       &pack_skip_rows) ||
        !query_integer(get_integerv, NXGL_FP_PACK_SKIP_PIXELS,
                       &pack_skip_pixels) ||
        pack_row_length != 0 || pack_skip_rows != 0 ||
        pack_skip_pixels != 0) {
      g_last_unmeasured_reason = "incompatible-pack-state";
      return 0;
    }
  }

  if ((size_t)width > SIZE_MAX / 4u) {
    g_last_unmeasured_reason = "readback-size-overflow";
    return 0;
  }
  row_bytes = (size_t)width * 4u;
  if (row_bytes > SIZE_MAX - (size_t)(alignment - 1)) {
    g_last_unmeasured_reason = "readback-size-overflow";
    return 0;
  }
  stride = (row_bytes + (size_t)(alignment - 1)) &
           ~((size_t)alignment - 1u);
  if ((size_t)height > SIZE_MAX / stride) {
    g_last_unmeasured_reason = "readback-size-overflow";
    return 0;
  }
  total = stride * (size_t)height;
  if (total == 0 || total > NXGL_FP_MAX_READBACK_BYTES) {
    g_last_unmeasured_reason = "readback-size-limit";
    return 0;
  }
  layout->row_bytes = row_bytes;
  layout->stride = stride;
  layout->total_bytes = total;
  return 1;
}

static unsigned char sentinel_byte(size_t offset, unsigned seed) {
  uint32_t value = (uint32_t)offset;
  value ^= seed * 0x9E3779B9u;
  value ^= value >> 16;
  value *= 0x7FEB352Du;
  value ^= value >> 15;
  return (unsigned char)value;
}

static void fill_sentinel(unsigned char *buffer, size_t len, unsigned seed) {
  for (size_t i = 0; i < len; i++)
    buffer[i] = sentinel_byte(i, seed);
}

static int readback_overwritten(const unsigned char *buffer, int height,
                                const frame_read_layout *layout,
                                unsigned seed) {
  size_t compared = 0, changed = 0;

  for (int y = 0; y < height; y++) {
    const unsigned char *row = buffer + (size_t)y * layout->stride;
    for (size_t x = 0; x < layout->row_bytes; x++) {
      size_t offset = (size_t)y * layout->stride + x;
      compared++;
      if (row[x] != sentinel_byte(offset, seed))
        changed++;
    }
  }
  /* A failed glReadPixels leaves the initialized sentinel untouched. Requiring
   * 99% replacement also refuses partial writes; a second, different sentinel
   * handles the vanishingly rare legitimate match without reading GL errors. */
  return compared > 0 && changed >= compared - compared / 100u;
}

static int guard_intact(const unsigned char *guard, size_t len) {
  for (size_t i = 0; i < len; i++)
    if (guard[i] != (unsigned char)0xD7)
      return 0;
  return 1;
}

static void record_unmeasured(const char *message) {
  g_unmeasured++;
  g_black_streak = 0;
  g_dead_streak = 0;
  g_last_unmeasured_reason = message;
  printf("gl: frame probe unavailable (%s)\n", message);
  fflush(stdout);
}

static void frame_proof_sample_locked(int width, int height) {
  frame_read_layout layout;
  nxgl_fp_get_integerv get_integerv;
  nxgl_fp_read_pixels read_pixels;
  unsigned char *buffer = NULL;
  const size_t guard_size = 64u;
  size_t allocation_size, pixels;
  size_t rgb_coloured = 0, visible = 0, opaque = 0, transparent = 0;
  double non_black, rgb_non_black;
  int before = g_pending_point == NXGL_PROOF_BEFORE_PRESENT;
  int read_ok = 0, preflight;

  if (g_fatal_active)
    return;

  /* A port without a carried drawable may use the current viewport. The
   * sentinel refuses a failed/unimplemented state query without glGetError. */
  if (width <= 0 || height <= 0) {
    int viewport[4] = {INT_MIN, INT_MIN, INT_MIN, INT_MIN};
    get_integerv = (nxgl_fp_get_integerv)resolve_gl("glGetIntegerv");
    if (get_integerv != NULL)
      get_integerv(NXGL_FP_VIEWPORT, viewport);
    width = viewport[2];
    height = viewport[3];
  }
  if (width <= 0 || height <= 0 || width > NXGL_FP_MAX_DIMENSION ||
      height > NXGL_FP_MAX_DIMENSION) {
    g_invalid_drawable++;
    record_unmeasured("invalid-drawable");
    return;
  }
  preflight = frame_read_preflight(width, height, &layout);
  if (preflight <= 0) {
    if (preflight < 0) {
      if (before) {
        g_dead_streak++;
        g_black_streak = 0;
      } else {
        g_dead_streak = 0;
        g_black_streak = 0;
      }
    } else {
      g_dead_streak = 0;
      record_unmeasured(g_last_unmeasured_reason);
    }
    return;
  }
  if (layout.total_bytes > SIZE_MAX - guard_size) {
    record_unmeasured("readback-size-overflow");
    return;
  }
  allocation_size = layout.total_bytes + guard_size;
  buffer = (unsigned char *)malloc(allocation_size);
  if (buffer == NULL) {
    record_unmeasured("allocation-failed");
    return;
  }
  read_pixels = (nxgl_fp_read_pixels)resolve_gl("glReadPixels");
  if (read_pixels == NULL) {
    free(buffer);
    record_unmeasured("glReadPixels-unresolved");
    return;
  }

  for (unsigned attempt = 1; attempt <= 2 && !read_ok; attempt++) {
    fill_sentinel(buffer, layout.total_bytes, attempt);
    memset(buffer + layout.total_bytes, 0xD7, guard_size);
    read_pixels(0, 0, width, height, NXGL_FP_RGBA,
                NXGL_FP_UNSIGNED_BYTE, buffer);
    if (!guard_intact(buffer + layout.total_bytes, guard_size)) {
      free(buffer);
      record_unmeasured("readback-overflow");
      return;
    }
    read_ok = readback_overwritten(buffer, height, &layout, attempt);
  }
  if (!read_ok) {
    free(buffer);
    record_unmeasured("readback-unproven");
    return;
  }

  if ((size_t)width > SIZE_MAX / (size_t)height) {
    free(buffer);
    record_unmeasured("pixel-count-overflow");
    return;
  }
  pixels = (size_t)width * (size_t)height;
  for (int y = 0; y < height; y++) {
    const unsigned char *row = buffer + (size_t)y * layout.stride;
    for (int x = 0; x < width; x++) {
      const unsigned char *pixel = row + (size_t)x * 4u;
      int rgb = pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0;
      if (rgb)
        rgb_coloured++;
      if (rgb && pixel[3] != 0)
        visible++;
      if (pixel[3] == 255)
        opaque++;
      else if (pixel[3] == 0)
        transparent++;
    }
  }
  non_black = (double)visible * 100.0 / (double)pixels;
  rgb_non_black = (double)rgb_coloured * 100.0 / (double)pixels;
  g_samples++;
  if (before)
    g_point_before++;
  else if (g_pending_point == NXGL_PROOF_AFTER_PRESENT)
    g_point_after++;
  else
    g_point_unspecified++;
  g_last_alpha0_pct = (double)transparent * 100.0 / (double)pixels;
  g_last_rgb_non_black = rgb_non_black;
  if (non_black > g_best_non_black)
    g_best_non_black = non_black;
  if (before && non_black > g_best_presented_non_black)
    g_best_presented_non_black = non_black;

  if (non_black >= FRAME_PROOF_MIN_NON_BLACK) {
    const char *proof_dir = getenv("NXLAUNCH_PROOF_DIR");
    g_black_streak = 0;
    g_dead_streak = 0;
    if (before && g_video_proof_state == VIDEO_PROOF_NONE) {
      if (!g_png_evidence_ready && proof_dir != NULL && proof_dir[0] != '\0') {
        g_png_evidence_ready =
            write_frame_png(proof_dir, width, height, layout.stride, buffer);
        g_png_required_failed = !g_png_evidence_ready;
      } else if (proof_dir == NULL || proof_dir[0] == '\0') {
        g_png_evidence_ready = 1;
        g_png_required_failed = 0;
      }
      if (g_png_evidence_ready)
        (void)publish_video_proof_file("OK", "non-black");
    }
  } else if (visible == 0) {
    if (before)
      g_black_streak++;
    else
      g_black_streak = 0;
    g_dead_streak = 0;
  } else {
    /* A sparse but genuinely visible frame is not proof of black output.
     * Loading/cutscene frames in FP2 can contain only a few hundred lit
     * pixels: keep them inconclusive until a later full frame proves OK, and
     * never let them extend a fatal all-black streak.  RGB with alpha zero is
     * still counted as visible==0 and remains a BLACK candidate. */
    g_black_streak = 0;
    g_dead_streak = 0;
  }

  printf("gl: frame probe %dx%d rgb_non_black=%.1f%% "
         "visible_non_black=%.1f%% alpha255=%.1f%% alpha0=%.1f%%\n",
         width, height, rgb_non_black, non_black,
         (double)opaque * 100.0 / (double)pixels,
         (double)transparent * 100.0 / (double)pixels);
  fflush(stdout);
  free(buffer);
}

void nxgl_frame_proof_sample(int width, int height) {
  if (!adapter_enter())
    return;
  frame_proof_sample_locked(width, height);
  adapter_unlock();
}


/* V3: explicit sampling point. Same probe as nxgl_frame_proof_sample; the
 * point is recorded with the sample and lands appended on the VIDEO line. */
void nxgl_frame_proof_sample_at(int width, int height,
                                nxgl_frame_proof_sample_point point) {
  if (point != NXGL_PROOF_BEFORE_PRESENT && point != NXGL_PROOF_AFTER_PRESENT) {
    fprintf(stderr, "gl: frame proof refused (invalid sample point)\n");
    fflush(stderr);
    return;
  }
  if (!adapter_enter())
    return;
  g_pending_point = (int)point;
  frame_proof_sample_locked(width, height);
  g_pending_point = -1;
  adapter_unlock();
}

static void frame_proof_publish_locked(void);

static void retry_fatal_receipt_locked(void) {
  if (g_video_proof_state == VIDEO_PROOF_FATAL_PENDING &&
      g_fatal_verdict != NULL && g_fatal_reason != NULL)
    (void)publish_video_proof_file(g_fatal_verdict, g_fatal_reason);
}

static void observe_fatal_streak_locked(unsigned long frame, int scream) {
  int conclusive = 0;
  const char *context;
  const char *verdict;
  const char *reason;
  int streak;

  if (g_dead_streak >= NXGL_FP_FATAL_STREAK) {
    verdict = "DEAD-CONTEXT";
    reason = "dead-context";
    streak = g_dead_streak;
  } else if (g_black_streak >= NXGL_FP_FATAL_STREAK) {
    verdict = "BLACK";
    reason = "black-streak";
    streak = g_black_streak;
  } else {
    retry_fatal_receipt_locked();
    return;
  }
  context = launch_context(&conclusive);
  if (conclusive) {
    if (!g_fatal_active) {
      g_fatal_close_pending = 1;
      __atomic_store_n(&g_fatal_active, 1, __ATOMIC_RELEASE);
    }
    (void)publish_video_proof_file(verdict, reason);
  }
  if (!scream || g_streak_screamed)
    return;
  g_streak_screamed = 1;
  printf("IMAGE PROOF: black-streak=%d frames=%lu launch=%s -- o jogo "
         "esta' vivo e o painel segue PRETO; ver o recibo VIDEO acima\n",
         streak, frame, context);
  printf("NXEVENT {\"schema\":\"nx-event-v1\",\"source\":\"gl\","
         "\"phase\":\"runtime\",\"status\":\"fail\",\"reason_code\":6304,"
         "\"details\":{\"image_proof\":\"%s\",\"streak\":%d,"
         "\"frames\":%lu,\"launch_context\":\"%s\",\"conclusive\":%s}}\n",
         strcmp(verdict, "DEAD-CONTEXT") == 0 ? "dead-context" :
                                                  "black-streak",
         streak, frame, context, conclusive ? "true" : "false");
  fflush(stdout);
}


/* Onda v2: prova de imagem continua. Uma chamada por quadro ANTES do present;
 * o cronograma esparso mantem o custo em um incremento por quadro fora das
 * amostras. Tres amostras SEGUIDAS pretas com os quadros andando = grito
 * IMAGE PROOF + NXEVENT 6304 (o caso Brotato: jogo vivo, renderer saudavel,
 * painel preto -- antes, nenhum log denunciava). */
void nxgl_frame_proof_before_present(int width, int height) {
  unsigned long frame;

  if (!adapter_enter())
    return;
  retry_fatal_receipt_locked();
  if (g_fatal_active) {
    adapter_unlock();
    return;
  }
  if (g_auto_proof_state == 0) {
    const char *flag = getenv("NXGL_IMAGE_PROOF");
    g_auto_proof_state =
        (flag != NULL && flag[0] == '0' && flag[1] == '\0') ? 2 : 1;
  }
  frame = ++g_frames_presented;
  if (g_auto_proof_state == 2) {
    adapter_unlock();
    return;
  }
  if (frame != 30ul && frame != 120ul && frame != 600ul &&
      (frame % 1800ul) != 0ul) {
    adapter_unlock();
    return;
  }

  g_pending_point = NXGL_PROOF_BEFORE_PRESENT;
  frame_proof_sample_locked(width, height);
  g_pending_point = -1;
  observe_fatal_streak_locked(frame, 1);
  if (frame == 600ul || (frame % 1800ul) == 0ul)
    frame_proof_publish_locked();
  adapter_unlock();
}


/* E2: the single-line receipt a human reads first. Additive -- the historical
 * "gl: frame proof verdict=" line and its NXEVENT stay untouched above. */
static void publish_video_receipt(const char *verdict, int conclusive) {
  char window[32], reason[48];
  if (g_ctx_set && g_ctx_w > 0 && g_ctx_h > 0)
    snprintf(window, sizeof window, "%dx%d", g_ctx_w, g_ctx_h);
  else
    snprintf(window, sizeof window, "?");
  if (strcmp(verdict, "OK") == 0)
    snprintf(reason, sizeof reason, "none");
  else if (strcmp(verdict, "UNMEASURED") == 0)
    snprintf(reason, sizeof reason, "%s", g_last_unmeasured_reason);
  else if (strcmp(verdict, "UNKNOWN") == 0)
    snprintf(reason, sizeof reason, g_invalid_drawable > 0
             ? "window-invalid" : "no-frames-sampled");
  else if (strcmp(verdict, "DEAD-CONTEXT") == 0)
    snprintf(reason, sizeof reason, "renderer-null-driverless-provider");
  else if (strcmp(verdict, "INCONCLUSIVE") == 0 && g_png_required_failed)
    snprintf(reason, sizeof reason, "proof-image-write-failed");
  else if (strcmp(verdict, "INCONCLUSIVE") == 0 &&
           g_best_non_black >= FRAME_PROOF_MIN_NON_BLACK &&
           g_best_presented_non_black < FRAME_PROOF_MIN_NON_BLACK)
    snprintf(reason, sizeof reason, "presentation-point-unproved");
  else if (strcmp(verdict, "INCONCLUSIVE") == 0 && conclusive)
    snprintf(reason, sizeof reason, "insufficient-black-streak");
  else if (g_last_alpha0_pct >= 99.0 &&
           g_last_rgb_non_black >= FRAME_PROOF_MIN_NON_BLACK)
    snprintf(reason, sizeof reason, "alpha-zero");
  else if (conclusive)
    snprintf(reason, sizeof reason, "all-black");
  else
    snprintf(reason, sizeof reason, "launch-cannot-prove");

  /* V3: name WHERE the counted samples were read. APPENDED field only --
   * everything before it stays byte-identical for existing log readers. */
  {
    const char *sample_point;
    int kinds = (g_point_before > 0) + (g_point_after > 0) +
                (g_point_unspecified > 0);
    if (kinds > 1)
      sample_point = "mixed";
    else if (g_point_before > 0)
      sample_point = "before-present";
    else if (g_point_after > 0)
      sample_point = "after-present";
    else if (g_point_unspecified > 0)
      sample_point = "unspecified";
    else
      sample_point = "none";

    if (g_samples > 0)
      printf("VIDEO: window=%s driver=%s renderer=%s gles=\"%s\" "
             "frame_proof=%.1f%% verdict=%s reason=%s sample_point=%s\n",
             window, g_ctx_set && g_ctx_driver[0] ? g_ctx_driver : "?",
             g_ctx_set && g_ctx_renderer[0] ? g_ctx_renderer : "?",
             g_ctx_set && g_ctx_version[0] ? g_ctx_version : "?",
             g_best_non_black, verdict, reason, sample_point);
    else
      printf("VIDEO: window=%s driver=%s renderer=%s gles=\"%s\" "
             "frame_proof=unsampled verdict=%s reason=%s sample_point=%s\n",
             window, g_ctx_set && g_ctx_driver[0] ? g_ctx_driver : "?",
             g_ctx_set && g_ctx_renderer[0] ? g_ctx_renderer : "?",
             g_ctx_set && g_ctx_version[0] ? g_ctx_version : "?",
             verdict, reason, sample_point);
  }
  fflush(stdout);
}

static void frame_proof_publish_locked(void) {
  /* Deliberately not one-shot. An automated run is killed rather than closed,
   * and it is killed at an arbitrary moment, so the log has to carry a current
   * verdict at all times instead of one that only appears if the run survives
   * to a specific frame. Readers take the last verdict line. */
  int conclusive = 0;
  const char *context = launch_context(&conclusive);
  int fatal_dead = g_dead_streak >= NXGL_FP_FATAL_STREAK;
  int fatal_black = g_black_streak >= NXGL_FP_FATAL_STREAK;
  int stored_fatal_dead =
      g_fatal_verdict != NULL &&
      strcmp(g_fatal_verdict, "DEAD-CONTEXT") == 0;
  int stored_fatal = g_video_proof_state == VIDEO_PROOF_FATAL_PENDING ||
                     g_video_proof_state == VIDEO_PROOF_FATAL_PUBLISHED;

  observe_fatal_streak_locked(g_frames_presented, 0);

  if ((fatal_dead && conclusive) || (stored_fatal && stored_fatal_dead)) {
    printf("gl: frame proof verdict=DEAD-CONTEXT samples=%d launch=%s "
           "(GL_RENDERER nulo em %d amostras consecutivas; provavel TELA PRETA "
           "com som -- provedor grafico sem driver)\n",
           g_samples, context, g_dead_streak);
    printf("NXEVENT {\"schema\":\"nx-event-v1\",\"source\":\"gl\","
           "\"phase\":\"runtime\",\"status\":\"fail\","
           "\"reason_code\":6303,\"details\":{\"frame_proof\":"
           "\"dead-context\",\"samples\":%d,\"launch_context\":"
           "\"%s\",\"conclusive\":%s}}\n",
           g_dead_streak, context, conclusive ? "true" : "false");
    fflush(stdout);
    publish_video_receipt("DEAD-CONTEXT", conclusive);
    return;
  }
  if ((fatal_black && conclusive) || (stored_fatal && !stored_fatal_dead)) {
    printf("gl: frame proof verdict=BLACK samples=%d "
           "best_presented_non_black=%.1f%% black_streak=%d launch=%s\n",
           g_samples, g_best_presented_non_black, g_black_streak, context);
    printf("NXEVENT {\"schema\":\"nx-event-v1\",\"source\":\"gl\","
           "\"phase\":\"runtime\",\"status\":\"fail\",\"reason_code\":6301,"
           "\"details\":{\"frame_proof\":\"black\",\"samples\":%d,"
           "\"best_non_black_pct\":%.1f,\"black_streak\":%d,"
           "\"launch_context\":\"%s\",\"conclusive\":%s}}\n",
           g_samples, g_best_presented_non_black, g_black_streak, context,
           conclusive ? "true" : "false");
    fflush(stdout);
    publish_video_receipt("BLACK", conclusive);
    return;
  }
  if (fatal_dead && !conclusive) {
    printf("gl: frame proof verdict=INCONCLUSIVE samples=0 "
           "dead_streak=%d launch=%s\n",
           g_dead_streak, context);
    fflush(stdout);
    publish_video_receipt("INCONCLUSIVE", conclusive);
    return;
  }
  if (g_samples <= 0) {
    if (g_published)
      return;
    g_published = 1;
    if (g_unmeasured > 0)
      /* The frames existed and the probe ran; the readback itself failed.
       * That is a harness defect, never evidence about the port. */
      printf("gl: frame proof verdict=UNMEASURED samples=0 attempts=%d "
             "launch=%s reason=%s\n",
             g_unmeasured, context, g_last_unmeasured_reason);
    else
      printf("gl: frame proof verdict=UNKNOWN samples=0 launch=%s "
             "(run ended before the first probe)\n",
             context);
    fflush(stdout);
    publish_video_receipt(g_unmeasured > 0 ? "UNMEASURED" : "UNKNOWN",
                          conclusive);
    return;
  }

  {
  double best_presented =
      g_best_presented_non_black < 0.0 ? 0.0 : g_best_presented_non_black;
  int proved = g_best_presented_non_black >= FRAME_PROOF_MIN_NON_BLACK &&
               !g_png_required_failed;
  int black = !proved;
  const char *verdict = proved ? "OK" : "INCONCLUSIVE";

  printf("gl: frame proof verdict=%s samples=%d best_presented_non_black=%.1f%% "
         "launch=%s\n",
         verdict, g_samples, best_presented, context);
  if (black && !conclusive)
    printf("gl: this launch cannot prove an image (launch=%s); re-test from "
           "the device frontend before blaming the port\n",
           context);
  printf("NXEVENT {\"schema\":\"nx-event-v1\",\"source\":\"gl\","
         "\"phase\":\"runtime\",\"status\":\"%s\",\"reason_code\":%d,"
         "\"details\":{\"frame_proof\":\"%s\",\"samples\":%d,"
         "\"best_non_black_pct\":%.1f,\"launch_context\":\"%s\","
         "\"conclusive\":%s}}\n",
         "ok", proved ? 6300 : 6302,
         proved ? "ok" : "inconclusive", g_samples,
         best_presented, context, conclusive ? "true" : "false");
  fflush(stdout);
  publish_video_receipt(verdict, conclusive);
  }
}

void nxgl_frame_proof_publish(void) {
  if (!adapter_enter())
    return;
  frame_proof_publish_locked();
  adapter_unlock();
}

/* nxgl 0.3.5: lock-free by design (see framework/nxgl CHANGELOG 0.3.5). The
 * frame loop polls this every frame while the render thread may hold the
 * guard inside a legitimate readback; failing closed returned 1 to a healthy
 * game and closed it with status 72. The flag only goes 0 -> 1 under the
 * guard, so an acquire load is the whole truth. */
int nxgl_frame_proof_is_fatal(void) {
  return __atomic_load_n(&g_fatal_active, __ATOMIC_ACQUIRE) != 0;
}

int nxgl_frame_proof_consume_fatal(void) {
  int pending;
  if (!adapter_enter())
    return 0;
  pending = g_fatal_close_pending;
  if (pending) {
    g_fatal_close_pending = 0;
    if (!revoke_health_receipt_locked()) {
      fprintf(stderr,
              "VIDEO-PROOF: fatal health receipt revocation refused\n");
      fflush(stderr);
    }
  }
  adapter_unlock();
  return pending;
}
