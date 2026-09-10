/* SPDX-License-Identifier: GPL-3.0-only */
/* nxinput_gptk_upgrade -- see include/nxinput_gptk_upgrade.h. */
#include "nxinput_gptk_upgrade.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define UPGRADE_BASENAME "NEXTOSCONTROLLERS.gptk"
#define UPGRADE_NEW_NAME "NEXTOSCONTROLLERS.gptk.new"
#define UPGRADE_TMP_NAME ".NEXTOSCONTROLLERS.gptk.new.tmp"

/* The SHA-256 used by the loader receipt, kept byte-identical in behavior so
 * a report and a receipt can be compared. */
typedef struct upgrade_sha {
  uint32_t state[8];
  uint64_t length;
  size_t used;
  unsigned char block[64];
} upgrade_sha;

static uint32_t sha_ror(uint32_t value, unsigned int bits) {
  return (value >> bits) | (value << (32u - bits));
}

static void sha_store_be32(unsigned char *out, uint32_t value) {
  out[0] = (unsigned char)(value >> 24);
  out[1] = (unsigned char)(value >> 16);
  out[2] = (unsigned char)(value >> 8);
  out[3] = (unsigned char)value;
}

static void sha_block(upgrade_sha *ctx, const unsigned char block[64]) {
  static const uint32_t k[64] = {
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
      0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
      0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
      0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
      0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
      0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
      0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
      0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
      0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
      0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
      0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
      0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
      0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
  uint32_t w[64];
  uint32_t a, b, c, d, e, f, g, h;
  unsigned int i;

  for (i = 0u; i < 16u; i++) {
    w[i] = ((uint32_t)block[i * 4u] << 24) |
           ((uint32_t)block[i * 4u + 1u] << 16) |
           ((uint32_t)block[i * 4u + 2u] << 8) |
           (uint32_t)block[i * 4u + 3u];
  }
  for (i = 16u; i < 64u; i++) {
    uint32_t s0 = sha_ror(w[i - 15u], 7) ^ sha_ror(w[i - 15u], 18) ^
                  (w[i - 15u] >> 3);
    uint32_t s1 = sha_ror(w[i - 2u], 17) ^ sha_ror(w[i - 2u], 19) ^
                  (w[i - 2u] >> 10);
    w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
  }
  a = ctx->state[0];
  b = ctx->state[1];
  c = ctx->state[2];
  d = ctx->state[3];
  e = ctx->state[4];
  f = ctx->state[5];
  g = ctx->state[6];
  h = ctx->state[7];
  for (i = 0u; i < 64u; i++) {
    uint32_t s1 = sha_ror(e, 6) ^ sha_ror(e, 11) ^ sha_ror(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = h + s1 + ch + k[i] + w[i];
    uint32_t s0 = sha_ror(a, 2) ^ sha_ror(a, 13) ^ sha_ror(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = s0 + maj;

    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  ctx->state[0] += a;
  ctx->state[1] += b;
  ctx->state[2] += c;
  ctx->state[3] += d;
  ctx->state[4] += e;
  ctx->state[5] += f;
  ctx->state[6] += g;
  ctx->state[7] += h;
}

static void upgrade_sha_hex(const char *data, size_t length,
                            char hex[NXINPUT_GPTK_UPGRADE_SHA_SIZE]) {
  static const char digits[] = "0123456789abcdef";
  upgrade_sha ctx;
  unsigned char digest[32];
  uint64_t bits;
  size_t i;

  memset(&ctx, 0, sizeof ctx);
  ctx.state[0] = 0x6a09e667u;
  ctx.state[1] = 0xbb67ae85u;
  ctx.state[2] = 0x3c6ef372u;
  ctx.state[3] = 0xa54ff53au;
  ctx.state[4] = 0x510e527fu;
  ctx.state[5] = 0x9b05688cu;
  ctx.state[6] = 0x1f83d9abu;
  ctx.state[7] = 0x5be0cd19u;
  for (i = 0u; i < length; i++) {
    ctx.block[ctx.used++] = (unsigned char)data[i];
    if (ctx.used == 64u) {
      sha_block(&ctx, ctx.block);
      ctx.used = 0u;
    }
  }
  bits = (uint64_t)length * 8u;
  ctx.block[ctx.used++] = 0x80u;
  if (ctx.used > 56u) {
    while (ctx.used < 64u) {
      ctx.block[ctx.used++] = 0u;
    }
    sha_block(&ctx, ctx.block);
    ctx.used = 0u;
  }
  while (ctx.used < 56u) {
    ctx.block[ctx.used++] = 0u;
  }
  for (i = 0u; i < 8u; i++) {
    ctx.block[63u - i] = (unsigned char)(bits >> (i * 8u));
  }
  sha_block(&ctx, ctx.block);
  for (i = 0u; i < 8u; i++) {
    sha_store_be32(digest + i * 4u, ctx.state[i]);
  }
  for (i = 0u; i < sizeof digest; i++) {
    hex[i * 2u] = digits[digest[i] >> 4];
    hex[i * 2u + 1u] = digits[digest[i] & 15u];
  }
  hex[64] = '\0';
}

const char *nxinput_gptk_upgrade_status_name(int status) {
  switch (status) {
    case NXINPUT_GPTK_UPGRADE_UP_TO_DATE:
      return "up-to-date";
    case NXINPUT_GPTK_UPGRADE_OWNER_MISSING:
      return "owner-missing";
    case NXINPUT_GPTK_UPGRADE_OFFERED:
      return "offered";
    case NXINPUT_GPTK_UPGRADE_REFUSED:
    default:
      return "refused";
  }
}


/* Read the owner's file through the directory descriptor only. A symlink or
 * a non-regular file is refused, never followed. */
static int read_owner(int owner_dir_fd, char *buffer, size_t cap,
                      size_t *length, int *present, const char **reason) {
  struct stat status;
  ssize_t got;
  size_t used = 0u;
  int fd;

  *present = 0;
  *length = 0u;
  fd = openat(owner_dir_fd, UPGRADE_BASENAME,
              O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) {
    return 0; /* absent (or a symlink, which O_NOFOLLOW turned into ELOOP) */
  }
  if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)) {
    (void)close(fd);
    *reason = "the owner's NEXTOSCONTROLLERS.gptk is not a regular file";
    return -1;
  }
  for (;;) {
    got = read(fd, buffer + used, cap - used);
    if (got < 0) {
      (void)close(fd);
      *reason = "the owner's NEXTOSCONTROLLERS.gptk is unreadable";
      return -1;
    }
    if (got == 0) {
      break;
    }
    used += (size_t)got;
    if (used == cap) {
      (void)close(fd);
      *reason = "the owner's NEXTOSCONTROLLERS.gptk exceeds the size limit";
      return -1;
    }
  }
  (void)close(fd);
  *length = used;
  *present = 1;
  return 0;
}

/* Bounded, path-free semantic diff over (context, control). */
static void build_diff(const nxinput_gptk *old_map, const nxinput_gptk *new_map,
                       nxinput_gptk_upgrade_report *report) {
  size_t used = 0u;
  int context;

  report->diff[0] = '\0';
  /* 0.10.0: FACE_LAYOUT is preamble state, invisible to decide(); report a
   * difference explicitly so the owner sees it in the offer. The owner's
   * file is still never overwritten -- this is reporting only. */
  if (nxinput_gptk_face_layout_of(old_map) !=
      nxinput_gptk_face_layout_of(new_map)) {
    int written = snprintf(
        report->diff, sizeof report->diff, "preamble FACE_LAYOUT: %s -> %s\n",
        nxinput_gptk_face_layout_name(
            (int)nxinput_gptk_face_layout_of(old_map)),
        nxinput_gptk_face_layout_name(
            (int)nxinput_gptk_face_layout_of(new_map)));
    if (written > 0) {
      used = (size_t)written;
    }
    report->changed++;
  }
  for (context = 0; context < (int)NXINPUT_GPTK_CONTEXT_COUNT; context++) {
    int control;

    for (control = 0; control < (int)NXINPUT_GPTK_CONTROL_COUNT; control++) {
      const char *old_action = 0;
      const char *new_action = 0;
      nxinput_gptk_decision old_decision = nxinput_gptk_decide(
          old_map, (nxinput_gptk_context)context, control, &old_action);
      nxinput_gptk_decision new_decision = nxinput_gptk_decide(
          new_map, (nxinput_gptk_context)context, control, &new_action);
      const char *old_text =
          old_decision == NXINPUT_GPTK_DECIDE_ACTION ? old_action
                                                     : nxinput_gptk_decision_name(
                                                           old_decision);
      const char *new_text =
          new_decision == NXINPUT_GPTK_DECIDE_ACTION ? new_action
                                                     : nxinput_gptk_decision_name(
                                                           new_decision);

      if (old_decision == new_decision &&
          (old_decision != NXINPUT_GPTK_DECIDE_ACTION ||
           strcmp(old_action, new_action) == 0)) {
        continue;
      }
      report->changed++;
      if (old_decision != NXINPUT_GPTK_DECIDE_ACTION &&
          old_decision != NXINPUT_GPTK_DECIDE_NATIVE &&
          (new_decision == NXINPUT_GPTK_DECIDE_ACTION ||
           new_decision == NXINPUT_GPTK_DECIDE_NATIVE)) {
        report->enabled++;
      }
      if ((old_decision == NXINPUT_GPTK_DECIDE_ACTION ||
           old_decision == NXINPUT_GPTK_DECIDE_NATIVE) &&
          new_decision == NXINPUT_GPTK_DECIDE_SUPPRESS) {
        report->disabled++;
      }
      if (used + 96u < sizeof report->diff) {
        int written = snprintf(report->diff + used,
                               sizeof report->diff - used, "%s %s: %s -> %s\n",
                               nxinput_gptk_context_name(context),
                               nxinput_gptk_control_name(control), old_text,
                               new_text);
        if (written > 0) {
          used += (size_t)written;
        }
      }
    }
  }
}

/* Create/replace the `.new` sibling atomically. */
static int write_new_atomic(int owner_dir_fd, const char *data, size_t length,
                           const char **reason) {
  struct stat status;
  size_t written = 0u;
  int fd;

  /* An existing `.new` that is a symlink is a trap, not a stale marker:
   * refuse visibly instead of replacing it. renameat() would never follow
   * it, but a reader of the offer might. */
  if (fstatat(owner_dir_fd, UPGRADE_NEW_NAME, &status,
              AT_SYMLINK_NOFOLLOW) == 0 &&
      !S_ISREG(status.st_mode)) {
    *reason = "the existing NEXTOSCONTROLLERS.gptk.new is not a regular file";
    return -1;
  }
  (void)unlinkat(owner_dir_fd, UPGRADE_TMP_NAME, 0);
  fd = openat(owner_dir_fd, UPGRADE_TMP_NAME,
              O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
  if (fd < 0) {
    *reason = "could not create the temporary upgrade file";
    return -1;
  }
  while (written < length) {
    ssize_t put = write(fd, data + written, length - written);

    if (put <= 0) {
      (void)close(fd);
      (void)unlinkat(owner_dir_fd, UPGRADE_TMP_NAME, 0);
      *reason = "could not write the upgrade file";
      return -1;
    }
    written += (size_t)put;
  }
  if (fsync(fd) != 0) {
    (void)close(fd);
    (void)unlinkat(owner_dir_fd, UPGRADE_TMP_NAME, 0);
    *reason = "could not flush the upgrade file";
    return -1;
  }
  (void)close(fd);
  /* renameat replaces the LINK; it never follows a symlink and cannot escape
   * the directory descriptor. */
  if (renameat(owner_dir_fd, UPGRADE_TMP_NAME, owner_dir_fd,
               UPGRADE_NEW_NAME) != 0) {
    (void)unlinkat(owner_dir_fd, UPGRADE_TMP_NAME, 0);
    *reason = "could not publish the upgrade file";
    return -1;
  }
  return 0;
}

int nxinput_gptk_upgrade_offer_at(int owner_dir_fd, const char *candidate,
                                  size_t candidate_length,
                                  nxinput_gptk_upgrade_report *report) {
  static char owner_text[NXINPUT_GPTK_MAX_BYTES + 1u];
  nxinput_gptk owner_map;
  nxinput_gptk candidate_map;
  const char *reason = "";
  size_t owner_length = 0u;
  int owner_present = 0;
  char parse_error[NXINPUT_GPTK_LOAD_ERROR_MAX];

  if (report == 0) {
    return -1;
  }
  memset(report, 0, sizeof *report);
  report->api_version = NXINPUT_GPTK_UPGRADE_API_VERSION;
  report->status = NXINPUT_GPTK_UPGRADE_REFUSED;
  if (owner_dir_fd < 0 || candidate == 0 ||
      candidate_length > (size_t)NXINPUT_GPTK_MAX_BYTES) {
    (void)snprintf(report->error, sizeof report->error,
                   "invalid upgrade request");
    return -1;
  }

  /* The candidate must itself be a valid map: an update never offers the
   * owner something the runtime would reject. */
  report->candidate_code = nxinput_gptk_parse(candidate, candidate_length,
                                              &candidate_map, parse_error,
                                              sizeof parse_error);
  report->candidate_bytes = candidate_length;
  upgrade_sha_hex(candidate, candidate_length, report->candidate_sha256);
  if (report->candidate_code != 0) {
    (void)snprintf(report->error, sizeof report->error,
                   "invalid candidate default: %.100s", parse_error);
    return 0;
  }

  if (read_owner(owner_dir_fd, owner_text, sizeof owner_text, &owner_length,
                 &owner_present, &reason) != 0) {
    (void)snprintf(report->error, sizeof report->error, "%s", reason);
    return 0;
  }
  report->owner_present = owner_present;
  if (!owner_present) {
    /* Materializing the FIRST copy is the launcher's job. An upgrade that
     * created it here would be indistinguishable from overwriting one. */
    report->status = NXINPUT_GPTK_UPGRADE_OWNER_MISSING;
    return 0;
  }
  report->owner_bytes = owner_length;
  upgrade_sha_hex(owner_text, owner_length, report->owner_sha256);
  if (owner_length == candidate_length &&
      memcmp(owner_text, candidate, owner_length) == 0) {
    report->status = NXINPUT_GPTK_UPGRADE_UP_TO_DATE;
    return 0;
  }

  report->owner_parsed = nxinput_gptk_parse(owner_text, owner_length,
                                            &owner_map, parse_error,
                                            sizeof parse_error);
  if (report->owner_parsed == 0) {
    build_diff(&owner_map, &candidate_map, report);
  } else {
    /* The owner's file no longer parses. It is STILL not touched: the offer
     * is written beside it and the owner decides. */
    (void)snprintf(report->diff, sizeof report->diff,
                   "owner file does not parse (NXI%04d); offered as-is\n",
                   report->owner_parsed);
  }

  if (write_new_atomic(owner_dir_fd, candidate, candidate_length, &reason) !=
      0) {
    (void)snprintf(report->error, sizeof report->error, "%s", reason);
    return 0;
  }
  report->status = NXINPUT_GPTK_UPGRADE_OFFERED;
  return 0;
}
