/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * nxinput_provider_linux -- the IMPURE half of the provider descriptor:
 * turns the address of the SDL entry point the process really calls into
 * nxinput_provider_evidence.
 *
 * Steps (5.1 of the V5 mission):
 *   1. dladdr(fn)            -> object base and pathname (advisory only);
 *   2. /proc/self/maps       -> the mapping that CONTAINS fn: dev, inode,
 *                               offset, [start,end);
 *   3. /proc/self/map_files/start-end  -> a FD to the object that is REALLY
 *                               mapped, whatever the path says now. Fallback:
 *                               open(pathname) and accept it ONLY if fstat
 *                               dev/inode equal the maps entry;
 *   4. sha256 of that FD, ELF Build ID note, DT_SONAME from the same bytes;
 *   5. dlopen(pathname, RTLD_NOLOAD) + dlsym of the four RetroArch-derived ById
 *      symbols; each must dladdr to the SAME base as fn (same object), or
 *      it does not count;
 *   6. SDL_GetVersion / SDL_GetRevision through dlsym on that same handle.
 *
 * The FD stays open across the sha and the pin decision (the caller closes
 * it via nxinput_provider_probe_close) so the bytes cannot change under the
 * decision. Nothing here uses ldconfig, LD_LIBRARY_PATH or a name.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "nxinput_provider_linux.h"

#include "nxinput_sha256.h"

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

typedef struct maps_hit {
  uintptr_t start, end;
  uint64_t offset;
  unsigned dev_major, dev_minor;
  uint64_t inode;
  char path[512];
} maps_hit;

static int find_mapping(uintptr_t addr, maps_hit *hit) {
  FILE *f = fopen("/proc/self/maps", "r");
  char line[1024];
  if (f == NULL) {
    return -1;
  }
  while (fgets(line, sizeof line, f) != NULL) {
    unsigned long long s, e, off, ino;
    unsigned maj, min;
    char perms[8];
    int consumed = 0;
    if (sscanf(line, "%llx-%llx %7s %llx %x:%x %llu%n", &s, &e, perms, &off,
               &maj, &min, &ino, &consumed) < 7) {
      continue;
    }
    if (addr >= (uintptr_t)s && addr < (uintptr_t)e) {
      const char *p = line + consumed;
      size_t n;
      while (*p == ' ') {
        p++;
      }
      n = strcspn(p, "\n");
      if (n >= sizeof hit->path) {
        n = sizeof hit->path - 1u;
      }
      memcpy(hit->path, p, n);
      hit->path[n] = '\0';
      hit->start = (uintptr_t)s;
      hit->end = (uintptr_t)e;
      hit->offset = off;
      hit->dev_major = maj;
      hit->dev_minor = min;
      hit->inode = ino;
      (void)fclose(f);
      return 0;
    }
  }
  (void)fclose(f);
  return -1;
}

/* Sanitized path class: never the path itself. */
static void classify_path(const char *path, char *out, size_t cap) {
  const char *cls = "other";
  if (path == NULL || path[0] == '\0') {
    cls = "anonymous";
  } else if (strstr(path, "/PortMaster/") != NULL ||
             strstr(path, "/portmaster/") != NULL) {
    cls = "portmaster-lib";
  } else if (strncmp(path, "/usr/lib", 8) == 0 ||
             strncmp(path, "/lib", 4) == 0 ||
             strncmp(path, "/usr/local/lib", 14) == 0) {
    cls = "system-lib";
  } else if (strstr(path, "/roms/") != NULL ||
             strstr(path, "/storage/") != NULL ||
             strstr(path, "/mnt/") != NULL) {
    cls = "port-tree-lib";
  }
  (void)snprintf(out, cap, "%s", cls);
}

static int open_mapped_object(const maps_hit *hit, int *out_fd,
                              uint8_t *bound) {
  char mf[96];
  struct stat sb;
  int fd;
  *bound = 0u;
  (void)snprintf(mf, sizeof mf, "/proc/self/map_files/%" PRIxPTR "-%" PRIxPTR,
                 hit->start, hit->end);
  fd = open(mf, O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    *bound = 1u; /* the kernel handed us the mapped object itself */
    *out_fd = fd;
    return 0;
  }
  if (hit->path[0] != '/') {
    return -1;
  }
  fd = open(hit->path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    fd = open(hit->path, O_RDONLY | O_CLOEXEC); /* symlinked SONAME */
  }
  if (fd < 0) {
    return -1;
  }
  if (fstat(fd, &sb) == 0 && (uint64_t)sb.st_ino == hit->inode) {
    if ((unsigned)major(sb.st_dev) == hit->dev_major &&
        (unsigned)minor(sb.st_dev) == hit->dev_minor) {
      *bound = 1u; /* same dev/inode as the mapping: the same object */
    } else if (hit->dev_major == 0u && major(sb.st_dev) == 0u) {
      /* Virtual/stacked filesystems (overlayfs, squashfs-in-overlay on
       * 4.4 kernels) report the LOWER device in /proc/self/maps and the
       * upper one in stat while the inode is the same object. Inode-bound
       * on a virtual device: accepted with reduced confidence (2). */
      *bound = 2u;
    }
  }
  *out_fd = fd;
  return 0;
}

static uint8_t *slurp(int fd, size_t *len) {
  struct stat sb;
  uint8_t *buf;
  size_t got = 0u;
  if (fstat(fd, &sb) != 0 || sb.st_size <= 0 || sb.st_size > (64 << 20)) {
    return NULL;
  }
  buf = (uint8_t *)malloc((size_t)sb.st_size);
  if (buf == NULL) {
    return NULL;
  }
  while (got < (size_t)sb.st_size) {
    ssize_t r = pread(fd, buf + got, (size_t)sb.st_size - got, (off_t)got);
    if (r <= 0) {
      free(buf);
      return NULL;
    }
    got += (size_t)r;
  }
  *len = got;
  return buf;
}

/* ELF Build ID note + DT_SONAME, from the file bytes, 32/64-bit. */
#define ELF_WALK(Ehdr, Shdr, Nhdr, Dyn)                                         \
  do {                                                                          \
    const Ehdr *eh = (const Ehdr *)buf;                                         \
    size_t i;                                                                   \
    if (len < sizeof *eh || eh->e_shoff == 0 ||                                 \
        eh->e_shoff + (size_t)eh->e_shnum * sizeof(Shdr) > len) {               \
      break;                                                                    \
    }                                                                           \
    for (i = 0; i < eh->e_shnum; i++) {                                         \
      const Shdr *sh = (const Shdr *)(buf + eh->e_shoff + i * sizeof(Shdr));    \
      if (sh->sh_offset + sh->sh_size > len) {                                  \
        continue;                                                               \
      }                                                                         \
      if (sh->sh_type == SHT_NOTE) {                                            \
        size_t off = sh->sh_offset, end = sh->sh_offset + sh->sh_size;          \
        while (off + sizeof(Nhdr) <= end) {                                     \
          const Nhdr *nh = (const Nhdr *)(buf + off);                           \
          size_t nameoff = off + sizeof(Nhdr);                                  \
          size_t descoff = nameoff + ((nh->n_namesz + 3u) & ~3u);               \
          if (descoff + nh->n_descsz > end) break;                              \
          if (nh->n_type == NT_GNU_BUILD_ID && nh->n_namesz == 4 &&             \
              memcmp(buf + nameoff, "GNU", 4) == 0 && nh->n_descsz <= 20) {     \
            size_t k;                                                           \
            for (k = 0; k < nh->n_descsz; k++) {                                \
              (void)snprintf(build_id + k * 2, 3, "%02x",                       \
                             (unsigned)buf[descoff + k]);                       \
            }                                                                   \
          }                                                                     \
          off = descoff + ((nh->n_descsz + 3u) & ~3u);                          \
        }                                                                       \
      } else if (sh->sh_type == SHT_DYNAMIC && sh->sh_link < eh->e_shnum) {     \
        const Shdr *strsh =                                                     \
            (const Shdr *)(buf + eh->e_shoff + sh->sh_link * sizeof(Shdr));     \
        size_t n = sh->sh_size / sizeof(Dyn), j;                                \
        for (j = 0; j < n; j++) {                                               \
          const Dyn *d = (const Dyn *)(buf + sh->sh_offset + j * sizeof(Dyn));  \
          if (d->d_tag == DT_SONAME &&                                          \
              strsh->sh_offset + d->d_un.d_val < len) {                         \
            (void)snprintf(soname, soname_cap, "%.*s", (int)(soname_cap - 1u), \
                           (const char *)buf + strsh->sh_offset +               \
                               d->d_un.d_val);                                  \
          }                                                                     \
        }                                                                       \
      }                                                                         \
    }                                                                           \
  } while (0)

static void elf_identity(const uint8_t *buf, size_t len, char *build_id,
                         char *soname, size_t soname_cap) {
  build_id[0] = '\0';
  soname[0] = '\0';
  if (len < EI_NIDENT || memcmp(buf, ELFMAG, SELFMAG) != 0) {
    return;
  }
  if (buf[EI_CLASS] == ELFCLASS64) {
    ELF_WALK(Elf64_Ehdr, Elf64_Shdr, Elf64_Nhdr, Elf64_Dyn);
  } else if (buf[EI_CLASS] == ELFCLASS32) {
    ELF_WALK(Elf32_Ehdr, Elf32_Shdr, Elf32_Nhdr, Elf32_Dyn);
  }
}

static int same_object(const void *a, const void *b_fn) {
  Dl_info ia, ib;
  if (b_fn == NULL) {
    return 0;
  }
  if (dladdr(a, &ia) == 0 || dladdr(b_fn, &ib) == 0) {
    return 0;
  }
  return ia.dli_fbase == ib.dli_fbase;
}

int nxinput_provider_probe_sdl(const void *sdl_entry, uint8_t api,
                               nxinput_provider_probe *probe) {
  maps_hit hit;
  Dl_info info;
  nxinput_provider_evidence *e;
  uint8_t *bytes = NULL;
  size_t len = 0u;
  int fd = -1;
  uint8_t bound = 0u;

  if (probe == NULL || sdl_entry == NULL) {
    return -1;
  }
  memset(probe, 0, sizeof *probe);
  probe->fd = -1;
  e = &probe->evidence;
  e->api_version = NXINPUT_PROVIDER_API_VERSION;
  e->struct_size = sizeof *e;
  e->api = api;

  memset(&hit, 0, sizeof hit);
  if (find_mapping((uintptr_t)sdl_entry, &hit) != 0) {
    (void)snprintf(e->path_class, sizeof e->path_class, "unmapped");
    return 0; /* evidence stays UNKNOWN-shaped */
  }
  e->map_dev = ((uint64_t)hit.dev_major << 32) | hit.dev_minor;
  e->map_inode = hit.inode;
  e->map_offset = hit.offset;
  classify_path(hit.path, e->path_class, sizeof e->path_class);

  if (dladdr(sdl_entry, &info) != 0 && info.dli_fname != NULL) {
    /* Statically linked provider: the entry lives in the MAIN PROGRAM.
     * 0.11.1 (review finding 5): decided against /proc/self/exe by
     * dev/inode, never against "the same object as nxinput" -- with the
     * glue vendored INTO a libSDL a DSO used to pass as static. */
    struct stat exe;
    if (stat("/proc/self/exe", &exe) == 0 &&
        (uint64_t)exe.st_ino == hit.inode &&
        (unsigned)major(exe.st_dev) == hit.dev_major &&
        (unsigned)minor(exe.st_dev) == hit.dev_minor) {
      e->statically_linked = 1u;
      (void)snprintf(e->path_class, sizeof e->path_class, "main");
    }
  }

  if (open_mapped_object(&hit, &fd, &bound) == 0) {
    bytes = slurp(fd, &len);
    if (bytes != NULL) {
      nxinput_sha256 ctx;
      uint8_t digest[32];
      nxinput_sha256_init(&ctx);
      nxinput_sha256_update(&ctx, bytes, len);
      nxinput_sha256_final(&ctx, digest);
      nxinput_sha256_hex(digest, e->sha256);
      e->sha_bound_to_mapping = bound;
      elf_identity(bytes, len, e->build_id, e->soname, sizeof e->soname);
      /* 0.11.1 (B8): an SDL2 ABI whose bytes carry the sdl2-compat marker
       * is a shim over the SDL3 core (the same process maps libSDL3 for it):
       * one provider behind two majors, never two isolated providers. */
      if (api == NXINPUT_SDL_API_2 && len > 12u) {
        static const char marker[] = "sdl2-compat.";
        size_t k;
        for (k = 0; k + sizeof marker - 1u <= len; k++) {
          if (bytes[k] == 's' && memcmp(bytes + k, marker, sizeof marker - 1u) == 0) { e->compat_over_sdl3 = 1u; break; }
        }
      }
      free(bytes);
    }
    probe->fd = fd; /* kept open until the caller closes the probe */
  }

  /* Exported RetroArch-derived table API, from the SAME object. */
  if (api == NXINPUT_SDL_API_2 && hit.path[0] == '/') {
    void *h = dlopen(hit.path, RTLD_NOLOAD | RTLD_NOW);
    if (h != NULL) {
      static const char *const names[4] = {
          "SDL_JoystickButtonEventCodeById", "SDL_JoystickAxisEventCodeById",
          "SDL_JoystickHatEventCodeById", "SDL_JoystickDevicePathById"};
      void *syms[4];
      int all = 1, i;
      for (i = 0; i < 4; i++) {
        syms[i] = dlsym(h, names[i]);
        if (syms[i] == NULL || !same_object(sdl_entry, syms[i])) {
          all = 0;
        }
      }
      if (all) {
        e->has_exported_bytable = 1u;
        probe->button_code_by_id = (int (*)(int, int))syms[0];
        probe->axis_code_by_id = (int (*)(int, int))syms[1];
        probe->hat_code_by_id = (int (*)(int, int))syms[2];
        probe->device_path_by_id = (const char *(*)(int))syms[3];
      }
      {
        void (*getver)(void *) = (void (*)(void *))dlsym(h, "SDL_GetVersion");
        const char *(*getrev)(void) =
            (const char *(*)(void))dlsym(h, "SDL_GetRevision");
        if (getver != NULL && same_object(sdl_entry, (void *)getver)) {
          uint8_t v[4] = {0, 0, 0, 0};
          getver(v);
          (void)snprintf(e->version, sizeof e->version, "%u.%u.%u", v[0],
                         v[1], v[2]);
        }
        if (getrev != NULL && same_object(sdl_entry, (void *)getrev)) {
          const char *r = getrev();
          if (r != NULL) {
            (void)snprintf(e->revision, sizeof e->revision, "%.*s",
                           (int)(sizeof e->revision - 1u), r);
          }
        }
      }
      (void)dlclose(h); /* NOLOAD handle: refcount only */
    }
  } else if (api == NXINPUT_SDL_API_3 && hit.path[0] == '/') {
    void *h = dlopen(hit.path, RTLD_NOLOAD | RTLD_NOW);
    if (h != NULL) {
      int (*getver)(void) = (int (*)(void))dlsym(h, "SDL_GetVersion");
      if (getver != NULL && same_object(sdl_entry, (void *)getver)) {
        int v = getver();
        (void)snprintf(e->version, sizeof e->version, "%d.%d.%d",
                       v / 1000000, (v / 1000) % 1000, v % 1000);
      }
      (void)dlclose(h);
    }
  }
  return 0;
}

void nxinput_provider_probe_close(nxinput_provider_probe *probe) {
  if (probe != NULL && probe->fd >= 0) {
    (void)close(probe->fd);
    probe->fd = -1;
  }
}

/* ---------------------------------------------- 0.11.1: measurement */
int nxinput_provider_measure_by_id(const nxinput_provider_probe *probe,
                                   int instance_id,
                                   nxinput_provider_measurement *out) {
  unsigned i;
  if (probe == NULL || out == NULL) {
    return -1;
  }
  memset(out, 0, sizeof *out);
  out->instance_id = instance_id;
  if (!probe->evidence.has_exported_bytable || probe->button_code_by_id == NULL ||
      probe->axis_code_by_id == NULL || probe->hat_code_by_id == NULL) {
    return -1;
  }
  for (i = 0; i < NXINPUT_PROVIDER_MEASURE_BUTTONS; i++) {
    int code = probe->button_code_by_id(instance_id, (int)i);
    if (code < 0) break;
    out->button_code[i] = code;
    out->buttons = i + 1u;
  }
  for (i = 0; i < NXINPUT_PROVIDER_MEASURE_AXES; i++) {
    int code = probe->axis_code_by_id(instance_id, (int)i);
    if (code < 0) break;
    out->axis_code[i] = code;
    out->axes = i + 1u;
  }
  for (i = 0; i < NXINPUT_PROVIDER_MEASURE_HATS; i++) {
    int code = probe->hat_code_by_id(instance_id, (int)i);
    if (code < 0) break;
    out->hat_code[i] = code;
    out->hats = i + 1u;
  }
  if (out->buttons == 0u && out->axes == 0u) {
    return -1; /* the provider does not know this instance (not open?) */
  }
  return 0;
}

/* ---------------------------------------------- 0.11.1: device probe */
static uint64_t fnv64(const void *d, size_t n, uint64_t h) {
  const unsigned char *p = d; size_t i;
  if (h == 0) h = 0xcbf29ce484222325ull;
  for (i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
  return h;
}

int nxinput_provider_probe_device(const char *devpath,
                                  unsigned long *key_bits, size_t key_words,
                                  unsigned long *abs_bits, size_t abs_words,
                                  nxinput_provider_device_probe *out) {
  struct stat sb;
  int fd;
  struct input_id id;
  char phys[128], uniq[128];
  if (out == NULL) {
    return -1;
  }
  memset(out, 0, sizeof *out);
  if (key_bits != NULL) memset(key_bits, 0, key_words * sizeof *key_bits);
  if (abs_bits != NULL) memset(abs_bits, 0, abs_words * sizeof *abs_bits);
  if (devpath == NULL || devpath[0] == '\0') {
    out->driver = NXINPUT_PROVIDER_DRIVER_VIRTUAL;
    return 0;
  }
  if (strncmp(devpath, "/dev/hidraw", 11) == 0) {
    out->driver = NXINPUT_PROVIDER_DRIVER_HIDAPI;
    out->physical_digest = fnv64(devpath, strlen(devpath), 0);
    return 0;
  }
  fd = open(devpath, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    out->driver = NXINPUT_PROVIDER_DRIVER_UNKNOWN;
    return 0;
  }
  if (fstat(fd, &sb) != 0 || !S_ISCHR(sb.st_mode)) {
    (void)close(fd);
    out->driver = NXINPUT_PROVIDER_DRIVER_UNKNOWN;
    return 0;
  }
  if (key_bits == NULL || abs_bits == NULL ||
      ioctl(fd, EVIOCGBIT(EV_KEY, key_words * sizeof *key_bits), key_bits) < 0 ||
      ioctl(fd, EVIOCGBIT(EV_ABS, abs_words * sizeof *abs_bits), abs_bits) < 0) {
    /* a character device that is not an evdev node: SDL reached it through
     * its HIDAPI driver (hidraw); the provider owns the mapping. */
    (void)close(fd);
    out->driver = NXINPUT_PROVIDER_DRIVER_HIDAPI;
    out->physical_digest = fnv64(devpath, strlen(devpath), 0);
    return 0;
  }
  out->driver = NXINPUT_PROVIDER_DRIVER_EVDEV;
  out->caps_measured = 1u;
  memset(&id, 0, sizeof id);
  if (ioctl(fd, EVIOCGID, &id) == 0) {
    out->bus = id.bustype; out->vendor = id.vendor; out->product = id.product; out->version = id.version;
  }
  phys[0] = '\0'; uniq[0] = '\0';
  (void)ioctl(fd, EVIOCGPHYS(sizeof phys - 1u), phys);
  (void)ioctl(fd, EVIOCGUNIQ(sizeof uniq - 1u), uniq);
  phys[sizeof phys - 1u] = '\0'; uniq[sizeof uniq - 1u] = '\0';
  (void)close(fd);
  {
    uint64_t h = fnv64(&id, sizeof id, 0);
    h = fnv64(phys, strlen(phys), h);
    h = fnv64(uniq, strlen(uniq), h);
    h = fnv64(key_bits, key_words * sizeof *key_bits, h);
    h = fnv64(abs_bits, abs_words * sizeof *abs_bits, h);
    out->physical_digest = h;
  }
  return 0;
}

/* ---------------------------------------------- 0.11.1: pin file */
static nxinput_provider_pin file_pins[NXINPUT_PROVIDER_PIN_FILE_MAX];
static char file_pin_text[NXINPUT_PROVIDER_PIN_FILE_MAX][160];

static uint8_t domain_by_name(const char *name) {
  int d;
  for (d = 1; d < (int)NXINPUT_SDL_DOMAIN_COUNT; d++) {
    if (strcmp(nxinput_sdl_domain_name((nxinput_sdl_domain)d), name) == 0) return (uint8_t)d;
  }
  return NXINPUT_SDL_DOMAIN_UNDECLARED;
}

int nxinput_provider_load_pin_file(const char *path) {
  FILE *f;
  char line[512];
  size_t n = 0u;
  nxinput_provider_set_runtime_pins(NULL, 0u);
  if (path == NULL || path[0] == '\0') {
    return -1;
  }
  f = fopen(path, "r");
  if (f == NULL) {
    return -1;
  }
  while (fgets(line, sizeof line, f) != NULL) {
    char sha[80], api[16], dom[48], id[64];
    size_t k;
    if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
    if (sscanf(line, "%79s %15s %47s %63s", sha, api, dom, id) != 4 || strlen(sha) != 64u) {
      (void)fclose(f);
      nxinput_provider_set_runtime_pins(NULL, 0u);
      return -1;
    }
    for (k = 0; k < 64u; k++) {
      if (!((sha[k] >= '0' && sha[k] <= '9') || (sha[k] >= 'a' && sha[k] <= 'f'))) { (void)fclose(f); return -1; }
    }
    if (n >= NXINPUT_PROVIDER_PIN_FILE_MAX) { (void)fclose(f); return -1; }
    if (domain_by_name(dom) == NXINPUT_SDL_DOMAIN_UNDECLARED && strcmp(dom, "undeclared") != 0) { (void)fclose(f); return -1; }
    (void)snprintf(file_pin_text[n], sizeof file_pin_text[n], "%s%c%s", sha, '\0', id);
    /* the text buffer holds "sha\0id": two C strings in one row */
    (void)snprintf(file_pin_text[n] + 65, sizeof file_pin_text[n] - 65u, "file:%s", id);
    file_pins[n].sha256 = file_pin_text[n];
    file_pins[n].id = file_pin_text[n] + 65;
    file_pins[n].api = strcmp(api, "sdl3") == 0 ? (uint8_t)NXINPUT_SDL_API_3 : (uint8_t)NXINPUT_SDL_API_2;
    file_pins[n].domain = domain_by_name(dom);
    n++;
  }
  (void)fclose(f);
  nxinput_provider_set_runtime_pins(file_pins, n);
  return (int)n;
}
