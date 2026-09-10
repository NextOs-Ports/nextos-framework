/* SPDX-License-Identifier: GPL-3.0-only */
/* 0.11.1 C6: the Godot NATIVE seam (joypad_linux, no SDL) and a CFW line
 * authored for the RetroArch-derived ASCENDING SDL2. The pad carries a low
 * key (KEY_VOLUMEDOWN 0x72) so the ascending and high-first numberings
 * differ: declared `sdl2-ascending-patched`, the line is translated by
 * physical code into the engine's high-first numbering and the engine's
 * readback agrees. MUTANT: the same bytes declared `sdl2-evdev` (the V4
 * import: ascending read as high-first) reach the engine with A/B/X/Y
 * shifted and the readback disagrees -> BLOCKED, never announced. */
#define _POSIX_C_SOURCE 200809L
#include "nxinput_godot_seam.h"
#include "nxinput_sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)
#define BPL (8u * (unsigned)sizeof(unsigned long))
#define KEY_WORDS (0x300u / BPL + 1u)
#define ABS_WORDS (0x40u / BPL + 1u)
static const char GUID[] = "0300000009120000a1c5000010010000";
/* keys: 0x72 (volume, a LOW key), 0x130..0x137 (south east c north west z tl tr) */
/* ascending numbering: b0=0x72 b1=0x130 b2=0x131 b3=0x132 b4=0x133 b5=0x134 b6=0x135 b7=0x136 b8=0x137 */
static const char ASC[] = "0300000009120000a1c5000010010000,Asc Pad,a:b2,b:b1,x:b4,y:b3,leftshoulder:b7,rightshoulder:b8,back:b5,start:b6,platform:Linux,";
/* what the engine (high-first: b0=0x130 ... b7=0x137, b8=0x72) must end up holding */
static const char HF[] = "0300000009120000a1c5000010010000,Asc Pad,a:b1,b:b0,x:b3,y:b2,leftshoulder:b6,rightshoulder:b7,back:b4,start:b5,platform:Linux,";
struct fake { int calls; char stored[1024]; unsigned seq; char last[2048]; int blocked_setter; };
static int op_add(void *u, const char *m) { struct fake *f = u; f->calls++; snprintf(f->stored, sizeof f->stored, "%s", m); return 0; }
static int op_has(void *u, int j) { struct fake *f = u; (void)j; return f->stored[0] != 0; }
/* The engine's HONEST readback of what it stored: parse its own mapping.
 * Godot logical order: a=0 b=1 x=2 y=3 L=4 R=5 L2=6 R2=7 L3=8 R3=9 SELECT=10 START=11. */
static int op_readback(void *u, int j, int physical) {
  struct fake *f = u; static const char *const names[] = {"a","b","x","y","leftshoulder","rightshoulder","lefttrigger","righttrigger","leftstick","rightstick","back","start"}; size_t i; (void)j;
  for (i = 0; i < sizeof names / sizeof names[0]; i++) { char pat[40]; const char *p; snprintf(pat, sizeof pat, ",%s:b%d,", names[i], physical); p = strstr(f->stored, pat); if (p) return (int)i; }
  return -1;
}
static uint64_t op_now(void *u) { struct fake *f = u; return (uint64_t)(++f->seq) * 1000u; }
static long op_pid(void *u) { (void)u; return 1; } static long op_tid(void *u) { (void)u; return 1; }
static void op_receipt(void *u, const char *l) { struct fake *f = u; size_t n = strlen(f->last); if (n + strlen(l) + 2 < sizeof f->last) { strcat(f->last, l); strcat(f->last, "\n"); } }
static unsigned long g_keys[KEY_WORDS], g_abs[ABS_WORDS]; static nxinput_godot_absinfo g_absinfo[0x40];
static void set_bit(unsigned long *b, unsigned c) { b[c / BPL] |= 1UL << (c % BPL); }
static const char *decl_file(const char *domain, const char *mapping, const char *sha) {
  static char path[256]; FILE *f; snprintf(path, sizeof path, "/tmp/nx-godot-asc-%ld.txt", (long)getpid()); f = fopen(path, "w"); if (!f) return NULL;
  fprintf(f, "domain=%s\nprovider=portmaster-gui\nreceipt=a578a7d82d47e681ad7a1cbe48bb49327dad6dbe1c808e46ca3485dbab0dae43\ngeneration=c3-nxinput-authority-v1\nguid=%s\nmapping_sha256=%s\nmapping=%s\n", domain, GUID, sha, mapping); fclose(f); return path;
}
int main(int argc, char **argv) {
  nxinput_godot_seam_engine e; nxinput_godot_seam_device d; struct fake f; const char *path; nxinput_godot_seam_result r;
  char sha[65];
  unsigned i;
  (void)argc; (void)argv;
  memset(&e, 0, sizeof e); e.api_version = NXINPUT_GODOT_SEAM_API_VERSION; e.struct_size = sizeof e; e.userdata = &f; e.major = 0u; e.add_joy_mapping = op_add; e.has_mapping = op_has; e.readback_logical_button = op_readback; e.monotonic_ns = op_now; e.pid = op_pid; e.tid = op_tid; e.receipt = op_receipt;
  set_bit(g_keys, 0x72); for (i = 0x130u; i <= 0x137u; i++) set_bit(g_keys, i);
  memset(&d, 0, sizeof d); d.api_version = NXINPUT_GODOT_SEAM_API_VERSION; d.struct_size = sizeof d; d.joy_id = 0; d.fd = 7; snprintf(d.guid, sizeof d.guid, "%s", GUID); snprintf(d.name, sizeof d.name, "Asc Pad"); snprintf(d.devpath, sizeof d.devpath, "/dev/input/event99");
  d.key_bits = g_keys; d.key_bit_count = 0x300u; d.abs_bits = g_abs; d.abs_bit_count = 0x40u; d.abs_info = g_absinfo; d.abs_info_count = 0x40u;
  { nxinput_sha256 ctx; uint8_t dg[32]; nxinput_sha256_init(&ctx); nxinput_sha256_update(&ctx, ASC, strlen(ASC)); nxinput_sha256_final(&ctx, dg); nxinput_sha256_hex(dg, sha); }
  /* 1. declared ascending: translated, engine holds the high-first line, readback agrees */
  memset(&f, 0, sizeof f); path = decl_file("sdl2-ascending-patched", ASC, sha);
  r = nxinput_godot_seam_admit(&e, &d, path);
  CHECK(r == NXINPUT_GODOT_SEAM_ADMIT, "ascending CFW line declared by its producer: ADMITTED by the Godot native seam");
  CHECK(f.calls == 1 && strstr(f.stored, ",a:b1,b:b0,x:b3,y:b2,") != NULL, "the engine received the TRANSLATED high-first line (a:b1 b:b0 x:b3 y:b2), never the ascending bytes");
  CHECK(strstr(f.last, "stage=translate result=ok translated_from=sdl2-ascending-patched to=sdl2-evdev") && strstr(f.last, "rewritten_bindings=8"), "receipt names the translation (8 bindings moved)");
  (void)HF;
  /* 2. MUTANT: the same bytes declared sdl2-evdev (V4 import): the engine stores them
   * verbatim and its readback shows the shift -> the seam must NOT announce */
  memset(&f, 0, sizeof f); path = decl_file("sdl2-evdev", ASC, sha);
  r = nxinput_godot_seam_admit(&e, &d, path);
  CHECK(r != NXINPUT_GODOT_SEAM_ADMIT, "MUTANT killed: ascending bytes imported as high-first (the V4 defect) -> engine readback disagrees, pad NOT announced");
  /* 3. unknown domain still blocked; digest still enforced */
  memset(&f, 0, sizeof f); path = decl_file("sdl2-ascending-patchedX", ASC, sha);
  CHECK(nxinput_godot_seam_admit(&e, &d, path) == NXINPUT_GODOT_SEAM_BLOCK_ORIGIN, "unknown domain: blocked (no default)");
  memset(&f, 0, sizeof f); path = decl_file("sdl2-ascending-patched", ASC, "0000000000000000000000000000000000000000000000000000000000000000");
  CHECK(nxinput_godot_seam_admit(&e, &d, path) == NXINPUT_GODOT_SEAM_BLOCK_ORIGIN, "digest mismatch on the DECLARED bytes: blocked before any translation");
  unlink(path);
  printf(fails ? "v5-godot-seam-ascending: FAIL\n" : "v5-godot-seam-ascending: OK\n"); return fails ? 1 : 0;
}
