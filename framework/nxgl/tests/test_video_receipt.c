/* SPDX-License-Identifier: GPL-3.0-only
 * Hermetic adapter harness. One scenario per process because adapter state is
 * deliberately process-global. No GL, window, device or network is used.
 */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../adapters/nxgl_frame_proof_adapter.h"

#define GL_VIEWPORT 0x0BA2u
#define GL_RENDERER 0x1F01u
#define GL_VERSION 0x1F02u
#define GL_EXTENSIONS 0x1F03u
#define GL_PACK_ALIGNMENT 0x0D05u
#define GL_PACK_ROW_LENGTH 0x0D02u
#define GL_PACK_SKIP_ROWS 0x0D03u
#define GL_PACK_SKIP_PIXELS 0x0D04u
#define GL_FRAMEBUFFER_BINDING 0x8CA6u
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAAu
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88EDu

enum read_mode {
  READ_NORMAL = 0,
  READ_NO_WRITE,
  READ_PARTIAL,
  READ_OVERFLOW,
  READ_REENTRANT
};

static unsigned char g_fill_r, g_fill_a = 255;
static unsigned g_gl_error;
static unsigned g_get_error_calls;
static unsigned g_read_calls;
static enum read_mode g_read_mode;
static int g_renderer_dead;
static const char *g_version = "OpenGL ES 3.2 fake";
static const char *g_extensions = "";
static int g_pack_alignment = 4;
static int g_pack_row_length;
static int g_pack_skip_rows;
static int g_pack_skip_pixels;
static int g_draw_fbo;
static int g_read_fbo;
static int g_pack_pbo;
static unsigned g_ignore_query;

static size_t fake_stride(int width) {
  size_t row = (size_t)width * 4u;
  size_t alignment = (size_t)g_pack_alignment;
  return (row + alignment - 1u) & ~(alignment - 1u);
}

static void fill_frame(int width, int height, unsigned char *data) {
  size_t stride = fake_stride(width);
  for (int y = 0; y < height; y++) {
    unsigned char *row = data + (size_t)y * stride;
    for (int x = 0; x < width; x++) {
      row[(size_t)x * 4u + 0u] = g_fill_r;
      row[(size_t)x * 4u + 1u] = 0;
      row[(size_t)x * 4u + 2u] = 0;
      row[(size_t)x * 4u + 3u] = g_fill_a;
    }
  }
}

static void fake_read_pixels(int x, int y, int width, int height,
                             unsigned format, unsigned type, void *data) {
  unsigned char *bytes = (unsigned char *)data;
  (void)x;
  (void)y;
  (void)format;
  (void)type;
  g_read_calls++;
  if (g_read_mode == READ_NO_WRITE)
    return;
  if (g_read_mode == READ_PARTIAL) {
    memset(bytes, 0, 16);
    return;
  }
  fill_frame(width, height, bytes);
  if (g_read_mode == READ_OVERFLOW)
    bytes[fake_stride(width) * (size_t)height] = 0;
  if (g_read_mode == READ_REENTRANT) {
    g_read_mode = READ_NORMAL;
    nxgl_frame_proof_publish();
  }
}

static unsigned fake_get_error(void) {
  unsigned error = g_gl_error;
  g_get_error_calls++;
  g_gl_error = 0;
  return error;
}

static const unsigned char *fake_get_string(unsigned name) {
  if (name == GL_RENDERER)
    return g_renderer_dead ? NULL : (const unsigned char *)"Fake-GPU";
  if (name == GL_VERSION)
    return (const unsigned char *)g_version;
  if (name == GL_EXTENSIONS)
    return (const unsigned char *)g_extensions;
  return NULL;
}

static void fake_get_integerv(unsigned name, int *value) {
  if (name == g_ignore_query)
    return;
  switch (name) {
  case GL_VIEWPORT:
    value[0] = 0;
    value[1] = 0;
    value[2] = 640;
    value[3] = 480;
    break;
  case GL_PACK_ALIGNMENT:
    *value = g_pack_alignment;
    break;
  case GL_PACK_ROW_LENGTH:
    *value = g_pack_row_length;
    break;
  case GL_PACK_SKIP_ROWS:
    *value = g_pack_skip_rows;
    break;
  case GL_PACK_SKIP_PIXELS:
    *value = g_pack_skip_pixels;
    break;
  case GL_FRAMEBUFFER_BINDING:
    *value = g_draw_fbo;
    break;
  case GL_READ_FRAMEBUFFER_BINDING:
    *value = g_read_fbo;
    break;
  case GL_PIXEL_PACK_BUFFER_BINDING:
    *value = g_pack_pbo;
    break;
  default:
    break;
  }
}

static void *fake_resolver(const char *name) {
  if (strcmp(name, "glReadPixels") == 0)
    return (void *)fake_read_pixels;
  if (strcmp(name, "glGetError") == 0)
    return (void *)fake_get_error;
  if (strcmp(name, "glGetString") == 0)
    return (void *)fake_get_string;
  if (strcmp(name, "glGetIntegerv") == 0)
    return (void *)fake_get_integerv;
  return NULL;
}

static void set_context(void) {
  nxgl_frame_proof_set_video_context(640, 480, "KMSDRM", "Fake-GPU",
                                     "OpenGL ES 3.2 fake");
}

static void sample_before(void) {
  nxgl_frame_proof_sample_at(640, 480, NXGL_PROOF_BEFORE_PRESENT);
}

static int write_collision(const char *path, const char *contents) {
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  size_t len = strlen(contents);
  if (fd < 0)
    return 0;
  if (write(fd, contents, len) != (ssize_t)len || close(fd) != 0)
    return 0;
  return 1;
}

static int file_is_empty(const char *path) {
  struct stat st;
  return lstat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 0;
}

static int collision_preserved(const char *path, const char *expected) {
  char buffer[64];
  int fd = open(path, O_RDONLY);
  ssize_t got;
  if (fd < 0)
    return 0;
  got = read(fd, buffer, sizeof buffer - 1u);
  if (close(fd) != 0 || got < 0)
    return 0;
  buffer[got] = '\0';
  return strcmp(buffer, expected) == 0;
}

int main(int argc, char **argv) {
  const char *scenario = argc > 1 ? argv[1] : "ok";
  int publish = 1;

  nxgl_frame_proof_set_resolver(fake_resolver);
  nxgl_frame_proof_launch_receipt();

  if (strcmp(scenario, "ok") == 0) {
    set_context();
    g_fill_r = 0;
    sample_before();
    g_fill_r = 200;
    sample_before();
  } else if (strcmp(scenario, "gl-error") == 0) {
    set_context();
    g_gl_error = 0x506u;
    g_read_mode = READ_NO_WRITE;
    sample_before();
  } else if (strcmp(scenario, "pending-error-visible") == 0) {
    set_context();
    g_gl_error = 0x502u;
    g_fill_r = 200;
    sample_before();
  } else if (strcmp(scenario, "partial-read") == 0) {
    set_context();
    g_read_mode = READ_PARTIAL;
    sample_before();
  } else if (strcmp(scenario, "overflow-read") == 0) {
    set_context();
    g_read_mode = READ_OVERFLOW;
    sample_before();
  } else if (strcmp(scenario, "dead-context") == 0) {
    set_context();
    g_renderer_dead = 1;
    g_fill_r = 200;
    sample_before();
    sample_before();
    sample_before();
  } else if (strcmp(scenario, "broken-dead-sequence") == 0) {
    set_context();
    g_renderer_dead = 1;
    sample_before();
    g_renderer_dead = 0;
    g_draw_fbo = 7;
    sample_before();
    g_draw_fbo = 0;
    g_renderer_dead = 1;
    sample_before();
    sample_before();
  } else if (strcmp(scenario, "alpha-zero") == 0) {
    set_context();
    g_fill_r = 200;
    g_fill_a = 0;
    sample_before();
    sample_before();
    sample_before();
  } else if (strcmp(scenario, "alpha-one") == 0) {
    set_context();
    g_fill_r = 200;
    g_fill_a = 1;
    sample_before();
  } else if (strcmp(scenario, "no-context") == 0) {
    g_fill_r = 200;
    sample_before();
  } else if (strcmp(scenario, "no-samples") == 0) {
    set_context();
  } else if (strcmp(scenario, "one-black") == 0) {
    set_context();
    g_fill_r = 0;
    sample_before();
  } else if (strcmp(scenario, "manual-three-black") == 0) {
    set_context();
    g_fill_r = 0;
    sample_before();
    sample_before();
    sample_before();
  } else if (strcmp(scenario, "unspecified-visible") == 0) {
    set_context();
    g_fill_r = 200;
    nxgl_frame_proof_sample(640, 480);
  } else if (strcmp(scenario, "after-visible") == 0) {
    set_context();
    g_fill_r = 200;
    nxgl_frame_proof_sample_at(640, 480, NXGL_PROOF_AFTER_PRESENT);
  } else if (strcmp(scenario, "fbo-bound") == 0) {
    set_context();
    g_draw_fbo = 7;
    sample_before();
  } else if (strcmp(scenario, "read-fbo-bound") == 0) {
    set_context();
    g_read_fbo = 7;
    sample_before();
  } else if (strcmp(scenario, "pbo-bound") == 0) {
    set_context();
    g_pack_pbo = 9;
    sample_before();
  } else if (strcmp(scenario, "pack-state") == 0) {
    set_context();
    g_pack_row_length = 1024;
    sample_before();
  } else if (strcmp(scenario, "pack-skip-rows") == 0) {
    set_context();
    g_pack_skip_rows = 1;
    sample_before();
  } else if (strcmp(scenario, "pack-skip-pixels") == 0) {
    set_context();
    g_pack_skip_pixels = 1;
    sample_before();
  } else if (strcmp(scenario, "query-unproven") == 0) {
    set_context();
    g_ignore_query = GL_READ_FRAMEBUFFER_BINDING;
    sample_before();
  } else if (strcmp(scenario, "pack8") == 0) {
    set_context();
    g_pack_alignment = 8;
    g_fill_r = 200;
    nxgl_frame_proof_sample_at(65, 63, NXGL_PROOF_BEFORE_PRESENT);
  } else if (strcmp(scenario, "gles1-default") == 0) {
    set_context();
    g_version = "OpenGL ES-CM 1.1 fake";
    g_fill_r = 200;
    sample_before();
  } else if (strcmp(scenario, "pack-alignment-invalid") == 0) {
    set_context();
    g_pack_alignment = 3;
    sample_before();
  } else if (strcmp(scenario, "readback-cap") == 0) {
    set_context();
    nxgl_frame_proof_sample_at(5000, 4000, NXGL_PROOF_BEFORE_PRESENT);
  } else if (strcmp(scenario, "oversize") == 0) {
    set_context();
    nxgl_frame_proof_sample_at(16385, 1, NXGL_PROOF_BEFORE_PRESENT);
  } else if (strcmp(scenario, "reentrant") == 0) {
    set_context();
    g_fill_r = 200;
    g_read_mode = READ_REENTRANT;
    sample_before();
  } else if (strcmp(scenario, "streak-black") == 0) {
    set_context();
    g_fill_r = 0;
    for (int i = 0; i < 700; i++)
      nxgl_frame_proof_before_present(640, 480);
    publish = 0;
  } else if (strcmp(scenario, "streak-ok") == 0) {
    set_context();
    g_fill_r = 200;
    for (int i = 0; i < 700; i++)
      nxgl_frame_proof_before_present(640, 480);
    publish = 0;
  } else if (strcmp(scenario, "streak-off") == 0) {
    g_fill_r = 0;
    for (int i = 0; i < 700; i++)
      nxgl_frame_proof_before_present(640, 480);
    publish = 0;
  } else if (strcmp(scenario, "ok-then-black") == 0) {
    set_context();
    g_fill_r = 200;
    for (int i = 0; i < 30; i++)
      nxgl_frame_proof_before_present(640, 480);
    g_fill_r = 0;
    for (int i = 30; i < 1801; i++)
      nxgl_frame_proof_before_present(640, 480);
    publish = 0;
  } else if (strcmp(scenario, "black-then-ok") == 0) {
    set_context();
    g_fill_r = 0;
    for (int i = 0; i < 700; i++)
      nxgl_frame_proof_before_present(640, 480);
    g_fill_r = 200;
    sample_before();
    publish = 0;
  } else if (strcmp(scenario, "retry-fatal") == 0) {
    const char *receipt = getenv("NXBOOTSTRAP_VIDEO_FILE");
    char collision[1400];
    set_context();
    g_fill_r = 200;
    sample_before();
    if (receipt == NULL ||
        snprintf(collision, sizeof collision, "%s.tmp.%ld.2", receipt,
                 (long)getpid()) <= 0 ||
        !write_collision(collision, "foreign-temp\n"))
      return 10;
    g_fill_r = 0;
    sample_before();
    sample_before();
    sample_before();
    nxgl_frame_proof_publish();
    if (!file_is_empty(receipt) ||
        !collision_preserved(collision, "foreign-temp\n"))
      return 11;
    printf("retry-safety: prior-ok=revoked foreign-temp=preserved\n");
    if (unlink(collision) != 0)
      return 12;
    nxgl_frame_proof_publish();
    publish = 0;
  } else if (strcmp(scenario, "contract-drift-fatal") == 0) {
    const char *receipt = getenv("NXBOOTSTRAP_VIDEO_FILE");
    char drift[1400];
    set_context();
    g_fill_r = 200;
    sample_before();
    if (receipt == NULL ||
        snprintf(drift, sizeof drift, "%s.drift", receipt) <= 0 ||
        setenv("NXBOOTSTRAP_VIDEO_FILE", drift, 1) != 0 ||
        setenv("NXBOOTSTRAP_HEALTH_RUN_ID", "attacker-changed-run", 1) != 0)
      return 16;
    g_fill_r = 0;
    sample_before();
    sample_before();
    sample_before();
    nxgl_frame_proof_publish();
    printf("contract-lock: ambient-drift=ignored original=authoritative\n");
    publish = 0;
  } else if (strcmp(scenario, "png-retry") == 0) {
    const char *dir = getenv("NXLAUNCH_PROOF_DIR");
    char collision[1400];
    set_context();
    if (dir == NULL ||
        snprintf(collision, sizeof collision,
                 "%s/frame-proof.png.tmp.%ld.1", dir, (long)getpid()) <= 0 ||
        !write_collision(collision, "foreign-png-temp\n"))
      return 13;
    g_fill_r = 200;
    sample_before();
    if (!collision_preserved(collision, "foreign-png-temp\n"))
      return 14;
    printf("png-retry: failed-write-not-claimed foreign-temp=preserved\n");
    if (unlink(collision) != 0)
      return 15;
    sample_before();
  } else {
    fprintf(stderr, "cenario desconhecido: %s\n", scenario);
    return 2;
  }

  if (publish)
    nxgl_frame_proof_publish();
  {
    int fatal = nxgl_frame_proof_is_fatal();
    int close = nxgl_frame_proof_consume_fatal();
    int close_again = nxgl_frame_proof_consume_fatal();
    printf("PROOF-LIFECYCLE: fatal=%d close=%d close_again=%d\n",
           fatal, close, close_again);
  }
  if (g_get_error_calls != 0) {
    fprintf(stderr, "adapter consumed glGetError %u time(s)\n",
            g_get_error_calls);
    return 20;
  }
  printf("GL-ERROR-QUEUE: untouched pending=0x%X reads=%u\n", g_gl_error,
         g_read_calls);
  return 0;
}
