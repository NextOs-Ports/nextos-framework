/* SPDX-License-Identifier: GPL-3.0-only */
/* V5 / B4-B5, C3-C4: the seam with the provider tail. Pure fake ops. */
#include "../../include/nxinput_sdl_seam.h"
#include <stdio.h>
#include <string.h>
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)
struct fake { const char *env_mapping; char held[900]; char last[900]; unsigned receipts; int hostile; uint8_t src_slot; };
static const char *f_getenv(void *u, const char *n) { struct fake *f = u; return strcmp(n, NXINPUT_AUTHORITY_ENV_MAPPING) == 0 ? f->env_mapping : 0; }
static int f_read(void *u, const char *p, char *o, size_t c) { (void)u; (void)p; (void)o; (void)c; return -1; }
static int f_add(void *u, const char *l) { struct fake *f = u; snprintf(f->held, sizeof f->held, "%s", l); return 0; }
static int f_guid(void *u, const char *g, char *o, size_t c) { struct fake *f = u; (void)g; if (!f->held[0]) return -1; if (f->hostile) { snprintf(o, c, "%.32s,Hostile,a:b9,platform:Linux,", f->held); return 0; } snprintf(o, c, "%s", f->held); return 0; }
static uint64_t f_now(void *u) { (void)u; return 1; } static long f_pid(void *u) { (void)u; return 1; } static long f_tid(void *u) { (void)u; return 1; }
static void f_rcpt(void *u, const char *l) { struct fake *f = u; f->receipts++; snprintf(f->last, sizeof f->last, "%s", l); }
static const char *f_getenv_bundle(void *u, const char *n) { struct fake *f = u; if (strcmp(n, NXINPUT_AUTHORITY_ENV_BUNDLE) == 0) return "/nonexistent/controllers.nxb"; return strcmp(n, NXINPUT_AUTHORITY_ENV_MAPPING) == 0 ? f->env_mapping : 0; }
static int f_norm_v5(void *u, uint8_t api, const char *g, const char *s, char *o, size_t c, unsigned *lines, unsigned *b) { struct fake *f = u; (void)api; (void)g; snprintf(o, c, "%s", s); *lines = 0; *b = 0; f->src_slot = NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED; return 0; }
static const char **g_stage_env; static int *g_stage_unset_calls; static int *g_stage_was_init;
static const char *stage_get(void *u, const char *n) { (void)u; return strcmp(n, NXINPUT_AUTHORITY_ENV_MAPPING) == 0 ? *g_stage_env : 0; }
static int stage_unset(void *u, const char *n) { (void)u; (void)n; (*g_stage_unset_calls)++; *g_stage_env = 0; return 0; }
static int stage_was_init(void *u) { (void)u; return *g_stage_was_init; }
#define LINE "19000000010000000100000000010000,Pad,a:b3,b:b4,x:b6,y:b5,back:b9,start:b10,leftx:a0,lefty:a1,platform:Linux,"
static void ops_init(nxinput_sdl_seam_ops *o, struct fake *f) { memset(o, 0, sizeof *o); o->api_version = NXINPUT_SDL_SEAM_API_VERSION; o->struct_size = sizeof *o; o->userdata = f; o->api = NXINPUT_SDL_API_2; o->getenv_fn = f_getenv; o->read_text_fn = f_read; o->add_mapping_fn = f_add; o->mapping_for_guid_fn = f_guid; o->monotonic_ns = f_now; o->pid = f_pid; o->tid = f_tid; o->receipt_fn = f_rcpt; }
static void dev_init(nxinput_sdl_seam_device *d, int id) { memset(d, 0, sizeof *d); d->api_version = NXINPUT_SDL_SEAM_API_VERSION; d->struct_size = sizeof *d; d->instance_id = id; strcpy(d->guid, "19000000010000000100000000010000"); snprintf(d->devpath, sizeof d->devpath, "/dev/input/event%d", id); d->buttons = 16; d->axes = 4; d->hats = 1; }
int main(void) {
  nxinput_sdl_seam seam; nxinput_sdl_seam_ops ops; nxinput_sdl_seam_device dev; struct fake f;
  /* 1. (0.11.1, review finding 1) UNKNOWN provider: the pad is ADMITTED in
   * STOCK MODE -- nothing translated, nothing external written to the store
   * by the seam; the provider's own mapping (env import / built-in) is read
   * back and reported. Before 0.11.1 this case BLOCKED the pad (every
   * unpinned CFW: ROCKNIX, ArkOS, AmberELEC, JELOS, rebuilt dArkOS/NextOS,
   * muOS 32-bit, SDL3) -- the RED that reproduced the mute pad. */
  memset(&seam, 0, sizeof seam); memset(&f, 0, sizeof f); f.env_mapping = LINE; ops_init(&ops, &f); ops.provider_domain = NXINPUT_SDL_DOMAIN_UNDECLARED; dev_init(&dev, 0);
  /* stock SDL already imported the env line itself: readback holds it */
  snprintf(f.held, sizeof f.held, "%s", LINE);
  { nxinput_sdl_seam_result r = nxinput_sdl_seam_admit(&seam, &ops, &dev);
    CHECK(r == NXINPUT_SDL_SEAM_ADMIT_STOCK, "MUTANT killed: UNKNOWN provider muting the pad -- admitted in stock mode (pad announced, provider's own mapping stays)"); }
  CHECK(strstr(f.last, "target_domain=undeclared") && !strstr(f.last, "target_domain=sdl2-evdev"), "receipt: target_domain=undeclared, not the major's presumption");
  CHECK(strstr(f.last, "source=stock-passthrough") && strstr(f.last, "decision=DO_NOT_MUTATE_STORE") && strstr(f.last, "passthrough=1") && strstr(f.last, "translated=0") && strstr(f.last, "mapping_present=1") && strstr(f.last, "env_reinstated=0"), "receipt: stock-passthrough, DO_NOT_MUTATE_STORE + passthrough, nothing translated, nothing reinstated");
  CHECK(strcmp(f.held, LINE) == 0, "store holds exactly what stock SDL imported (the seam wrote nothing)");
  /* 1b. UNKNOWN provider, nothing anywhere (no env, no builtin): still admitted
   * (SDL decides IsGameController itself), receipt says mapping_present=0. */
  memset(&seam, 0, sizeof seam); memset(&f, 0, sizeof f); f.env_mapping = LINE; ops_init(&ops, &f); ops.provider_domain = NXINPUT_SDL_DOMAIN_UNDECLARED; dev_init(&dev, 0);
  { nxinput_sdl_seam_result r = nxinput_sdl_seam_admit(&seam, &ops, &dev);
    CHECK(r == NXINPUT_SDL_SEAM_ADMIT_STOCK && strstr(f.last, "mapping_present=0") && f.held[0] == '\0', "UNKNOWN + no stock mapping: announced, store untouched, receipt honest (never blocked)"); }
  /* 1c. LEGACY blind staging removed the CFW's own env line before the
   * descriptor existed: the seam hands THAT line (and only that source) back
   * to the setter untranslated -- EXISTING_NATIVE_PASSTHROUGH of 1.4. */
  memset(&seam, 0, sizeof seam); memset(&f, 0, sizeof f); f.env_mapping = 0; ops_init(&ops, &f); ops.provider_domain = NXINPUT_SDL_DOMAIN_UNDECLARED; ops.staged_mapping = "deadbeef00000000000000000000beef,Other,a:b0,\n" LINE "\n"; dev_init(&dev, 0);
  { nxinput_sdl_seam_result r = nxinput_sdl_seam_admit(&seam, &ops, &dev);
    CHECK(r == NXINPUT_SDL_SEAM_ADMIT_STOCK && strcmp(f.held, LINE) == 0 && strstr(f.last, "env_reinstated=1") && strstr(f.last, "env_reinstated_lines=1") && strstr(f.last, "mapping_present=1"), "legacy staging + UNKNOWN: the CFW's own env line for this GUID is reinstated untranslated (only that GUID's line)"); }
  /* 1d. MUTANT: a port BUNDLE under UNKNOWN never reaches the setter, even
   * when nothing else exists (external line, not native by provenance). */
  memset(&seam, 0, sizeof seam); memset(&f, 0, sizeof f); f.env_mapping = 0; ops_init(&ops, &f); ops.provider_domain = NXINPUT_SDL_DOMAIN_UNDECLARED; dev_init(&dev, 0);
  { static const char *bundle_env = "/nonexistent/controllers.nxb";
    struct fake_b { const char *env_mapping; char held[900]; char last[900]; unsigned receipts; int hostile; uint8_t src_slot; } *fb = (struct fake_b *)&f; (void)fb; (void)bundle_env;
    f.env_mapping = 0;
    { nxinput_sdl_seam_result r; ops.getenv_fn = f_getenv_bundle; r = nxinput_sdl_seam_admit(&seam, &ops, &dev);
      CHECK(r == NXINPUT_SDL_SEAM_ADMIT_STOCK && f.held[0] == '\0' && strstr(f.last, "bundle_ignored=1"), "MUTANT killed: bundle line reaching the setter under an UNKNOWN provider (ignored, receipt says so, pad still announced)"); } }
  /* 1e. HIDAPI device (hidraw): no evdev caps, provider table irrelevant:
   * admitted in stock mode with the provider's own HIDAPI mapping. */
  memset(&seam, 0, sizeof seam); memset(&f, 0, sizeof f); f.env_mapping = LINE; ops_init(&ops, &f); ops.provider_domain = NXINPUT_SDL_DOMAIN_SDL2_EVDEV; dev_init(&dev, 3);
  dev.driver = NXINPUT_PROVIDER_DRIVER_HIDAPI; dev.buttons = -1; dev.axes = -1; dev.hats = -1; snprintf(dev.devpath, sizeof dev.devpath, "/dev/hidraw3");
  snprintf(f.held, sizeof f.held, "%.32s,DualShock 4,a:b0,b:b1,platform:Linux,", LINE);
  { nxinput_sdl_seam_result r = nxinput_sdl_seam_admit(&seam, &ops, &dev);
    CHECK(r == NXINPUT_SDL_SEAM_ADMIT_STOCK && strstr(f.last, "source=provider-native-hidapi") && strstr(f.last, "driver=hidapi") && strstr(f.held, "DualShock"), "MUTANT killed: HIDAPI pad blocked as device-record-not-usable (admitted, provider's own mapping kept)"); }
  /* 1f. 0.11.6: a device the glue could NOT measure -- no devpath from the
   * provider (VIRTUAL) or a node this user cannot open (UNKNOWN) -- is the
   * provider's pad all the same: admitted in STOCK mode, never blocked. */
  memset(&seam, 0, sizeof seam); memset(&f, 0, sizeof f); f.env_mapping = LINE; ops_init(&ops, &f); ops.provider_domain = NXINPUT_SDL_DOMAIN_SDL2_EVDEV; dev_init(&dev, 4);
  dev.driver = NXINPUT_PROVIDER_DRIVER_VIRTUAL; dev.buttons = -1; dev.axes = -1; dev.hats = -1; dev.devpath[0] = '\0';
  { nxinput_sdl_seam_result r = nxinput_sdl_seam_admit(&seam, &ops, &dev);
    CHECK(r == NXINPUT_SDL_SEAM_ADMIT_STOCK && strstr(f.last, "driver=virtual") && strstr(f.last, "source=stock-passthrough"), "MUTANT killed: pad without a devpath (VIRTUAL) blocked as device-record-not-usable -- stock mode, never mute"); }
  memset(&seam, 0, sizeof seam); memset(&f, 0, sizeof f); f.env_mapping = LINE; ops_init(&ops, &f); ops.provider_domain = NXINPUT_SDL_DOMAIN_SDL2_EVDEV; dev_init(&dev, 5);
  dev.driver = NXINPUT_PROVIDER_DRIVER_UNKNOWN; dev.buttons = -1; dev.axes = -1; dev.hats = -1; snprintf(dev.devpath, sizeof dev.devpath, "/dev/input/event9");
  { nxinput_sdl_seam_result r = nxinput_sdl_seam_admit(&seam, &ops, &dev);
    CHECK(r == NXINPUT_SDL_SEAM_ADMIT_STOCK && strstr(f.last, "driver=unknown"), "MUTANT killed: pad whose node cannot be opened (EACCES/race/symlink) blocked -- stock mode, never mute"); }
  /* a MEASURED device with zero buttons and zero axes is still not a pad */
  memset(&seam, 0, sizeof seam); memset(&f, 0, sizeof f); f.env_mapping = LINE; ops_init(&ops, &f); ops.provider_domain = NXINPUT_SDL_DOMAIN_SDL2_EVDEV; dev_init(&dev, 6);
  dev.driver = NXINPUT_PROVIDER_DRIVER_EVDEV; dev.buttons = 0; dev.axes = 0; dev.hats = 0;
  CHECK(nxinput_sdl_seam_admit(&seam, &ops, &dev) == NXINPUT_SDL_SEAM_BLOCK_IDENTITY, "measured evdev node with no buttons and no axes stays blocked (teeth kept)");
  /* 2. V5 tail: provider domain + proved source domain slot in the receipt. */
  memset(&seam, 0, sizeof seam); memset(&f, 0, sizeof f); f.env_mapping = LINE; ops_init(&ops, &f); ops.provider_domain = NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED; ops.normalize_source_fn = f_norm_v5; ops.source_domain_slot = &f.src_slot; dev_init(&dev, 0);
  CHECK(nxinput_sdl_seam_admit(&seam, &ops, &dev) == NXINPUT_SDL_SEAM_ADMIT && strstr(f.last, "source_domain=sdl2-ascending-patched target_domain=sdl2-ascending-patched"), "receipt names the proved source domain and the provider domain");
  /* 3. 0.10.0-sized ops table (no V5 tail) still works; target reads undeclared. */
  memset(&seam, 0, sizeof seam); memset(&f, 0, sizeof f); f.env_mapping = LINE; ops_init(&ops, &f); ops.struct_size = NXINPUT_SDL_SEAM_OPS_SIZE_0_10_0; dev_init(&dev, 0);
  CHECK(nxinput_sdl_seam_admit(&seam, &ops, &dev) == NXINPUT_SDL_SEAM_ADMIT && strstr(f.last, "target_domain=undeclared"), "0.10.0 ops layout: compatible, target honestly undeclared");
  /* 4. C4: setter ok + hostile readback => NOT admitted (quarantine), store
   * decision never published. */
  memset(&seam, 0, sizeof seam); memset(&f, 0, sizeof f); f.env_mapping = LINE; f.hostile = 1; ops_init(&ops, &f); ops.provider_domain = NXINPUT_SDL_DOMAIN_SDL2_EVDEV; dev_init(&dev, 0);
  CHECK(nxinput_sdl_seam_admit(&seam, &ops, &dev) != NXINPUT_SDL_SEAM_ADMIT, "setter accepted but readback divergent: refused before announce");
  /* 5. C3: same GUID, divergent body, second instance refused; first intact. */
  memset(&seam, 0, sizeof seam); memset(&f, 0, sizeof f); f.env_mapping = LINE; ops_init(&ops, &f); ops.provider_domain = NXINPUT_SDL_DOMAIN_SDL2_EVDEV; dev_init(&dev, 0);
  CHECK(nxinput_sdl_seam_admit(&seam, &ops, &dev) == NXINPUT_SDL_SEAM_ADMIT, "first instance admitted");
  f.env_mapping = "19000000010000000100000000010000,Other,a:b4,b:b3,back:b9,start:b10,platform:Linux,"; dev_init(&dev, 1);
  { nxinput_sdl_seam_result r = nxinput_sdl_seam_admit(&seam, &ops, &dev); CHECK(r != NXINPUT_SDL_SEAM_ADMIT, "same-GUID divergent second pad refused before mutating the store"); }
  CHECK(strcmp(f.held, LINE) == 0, "store still holds the first pad's line");
  /* 6. (0.11.1) staging that knows the provider: UNKNOWN => the env line is
   * LEFT for the stock import; a provider with a table => staged as before. */
  { static const char *envval; static int unset_calls; static int was_init;
    struct e { int dummy; } eu;
    nxinput_sdl_seam_env_ops env; char buf[600]; size_t len = 99; int left = -1; nxinput_provider_descriptor pd;
    const char *(*get)(void *, const char *) = 0; (void)get; (void)eu;
    envval = LINE; unset_calls = 0; was_init = 0;
    memset(&env, 0, sizeof env); env.api_version = NXINPUT_SDL_SEAM_API_VERSION; env.struct_size = sizeof env;
    env.getenv_fn = stage_get; env.unsetenv_fn = stage_unset; env.sdl_was_init_fn = stage_was_init; env.userdata = 0;
    g_stage_env = &envval; g_stage_unset_calls = &unset_calls; g_stage_was_init = &was_init;
    memset(&pd, 0, sizeof pd); pd.method = NXINPUT_PROVIDER_METHOD_UNKNOWN; pd.domain = NXINPUT_SDL_DOMAIN_UNDECLARED;
    CHECK(nxinput_sdl_seam_stage_with_provider(&env, &pd, buf, sizeof buf, &len, &left) == 0 && left == 1 && len == 0 && unset_calls == 0 && envval != 0, "MUTANT killed: unsetenv(SDL_GAMECONTROLLERCONFIG) before a descriptor exists -- UNKNOWN provider leaves the CFW line in the environment");
    pd.method = NXINPUT_PROVIDER_METHOD_PINNED_ELF; pd.domain = NXINPUT_SDL_DOMAIN_SDL2_EVDEV; left = -1;
    CHECK(nxinput_sdl_seam_stage_with_provider(&env, &pd, buf, sizeof buf, &len, &left) == 0 && left == 0 && len == strlen(LINE) && unset_calls == 1 && strcmp(buf, LINE) == 0, "provider with a table: staged out of the environment (as 0.10.0 did)");
    pd.method = NXINPUT_PROVIDER_METHOD_EXPORTED_API; pd.domain = NXINPUT_SDL_DOMAIN_UNDECLARED; envval = LINE; left = -1;
    CHECK(nxinput_sdl_seam_stage_with_provider(&env, &pd, buf, sizeof buf, &len, &left) == 0 && left == 1, "EXPORTED_API without a measured table is still UNKNOWN for staging (left for stock)");
    was_init = 1;
    CHECK(nxinput_sdl_seam_stage_with_provider(&env, &pd, buf, sizeof buf, &len, &left) == -1, "too late (SDL initialised): refused"); }
  /* 0.11.4 (review 2, N2): the env line was LEFT for stock before init
   * (provider undecided then); the provider gets decided later (in-process
   * measurement). The run stays in STOCK mode: no setter call, the store
   * holds what SDL imported, the receipt never says "rewritten". */
  memset(&seam, 0, sizeof seam); memset(&f, 0, sizeof f); f.env_mapping = LINE; snprintf(f.held, sizeof f.held, "%s", LINE); ops_init(&ops, &f);
  ops.provider_domain = NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED; ops.provider_method = NXINPUT_PROVIDER_METHOD_MEASURED_INPROCESS; ops.staged_mapping = 0; ops.env_left_for_stock = 1; dev_init(&dev, 40);
  { nxinput_sdl_seam_result r = nxinput_sdl_seam_admit(&seam, &ops, &dev);
    CHECK(r == NXINPUT_SDL_SEAM_ADMIT_STOCK && strcmp(f.held, LINE) == 0 && strstr(f.last, "source=stock-passthrough"), "MUTANT killed: provider decided AFTER the env line was left for stock -> seam flipped to provider mode (USER-priority line cannot be outranked; must stay stock)"); }
  /* the same descriptor with the line STAGED (removed before init) is provider mode, as before */
  memset(&seam, 0, sizeof seam); memset(&f, 0, sizeof f); f.env_mapping = 0; ops_init(&ops, &f);
  ops.provider_domain = NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED; ops.provider_method = NXINPUT_PROVIDER_METHOD_MEASURED_INPROCESS; ops.staged_mapping = LINE "\n"; ops.env_left_for_stock = 0; ops.normalize_source_fn = f_norm_v5; ops.source_domain_slot = &f.src_slot; dev_init(&dev, 41);
  { nxinput_sdl_seam_result r = nxinput_sdl_seam_admit(&seam, &ops, &dev);
    CHECK(r != NXINPUT_SDL_SEAM_ADMIT_STOCK, "staged line + decided provider: provider mode (the flag only matters for a line left in the environment)"); }
  printf(fails ? "v5-seam: FAIL\n" : "v5-seam: OK\n"); return fails ? 1 : 0;
}
