/* ab_lua_control.c — snapshot C -> Lua para o controle nativo do estilingue.
 *
 * O build mobile não lê EVENT_CONTROLLER_AXIS_UPDATE no gameplay. Em vez de
 * fabricar um toque em coordenadas de tela, Slingshot.lua chama uma função Lua
 * original que não é usada pelo jogo normal (os.clock). Substituímos somente
 * essa entrada para devolver o estado coerente do pad. O adapter Lua continua
 * usando os métodos públicos originais do SlingshotSystem.
 *
 * Contrato auditado para as duas bibliotecas oficiais 8.0.3:
 * - 22680302: 049fc3739ff9075b3ad1557597fa6a40e5f242c4fb761855ce50b4ff2a67df84
 * - 8031:     ce7a5179cd00fce1f6c3c1ccf56e98dd87735ea56d814d962f0dc733b22988f3
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <SDL.h>
#include <stdint.h>

#include "ab_port.h"

#define SF __attribute__((pcs("aapcs")))
typedef void(SF *LuaPushNumberFn)(void *state, float value);
typedef void(SF *LuaPushBooleanFn)(void *state, int value);

typedef struct AbLuaProfile {
  const char *name;
  uintptr_t os_clock_vma;
  uintptr_t push_number_vma;
  uintptr_t push_boolean_vma;
} AbLuaProfile;

static const AbLuaProfile k_lua_profiles[] = {
    {"8.0.3-22680302", 0x65ace4u, 0x7ce6f4u, 0x7ce990u},
    {"8.0.3-8031", 0x659944u, 0x7cd354u, 0x7cd5f0u},
};

static LuaPushNumberFn g_push_number;
static LuaPushBooleanFn g_push_boolean;
static SDL_mutex *g_lock;
static float g_x, g_y;
static int g_blocked;
static int g_launch_latch, g_l1_latch, g_r1_latch, g_triangle_latch;
static Uint32 g_launch_tick, g_l1_tick, g_r1_tick, g_triangle_tick;
static Uint32 g_gameplay_tick;

#define BUTTON_LATCH_TTL_MS 500u

static int SF control_rpc(void *state) {
  float x, y;
  int blocked, launch, l1, r1, triangle;
  Uint32 now = SDL_GetTicks();

  /* Este RPC é chamado somente por SlingshotSystem.gameUpdate, depois que o
   * HUD jogável existe. A validade curta separa gameplay de menus sem ler
   * memória privada nem inventar uma segunda entrada no fluxo do jogo. */
  g_gameplay_tick = now;

  SDL_LockMutex(g_lock);
  x = g_x;
  y = g_y;
  blocked = g_blocked;
  launch = g_launch_latch && now - g_launch_tick <= BUTTON_LATCH_TTL_MS;
  l1 = g_l1_latch && now - g_l1_tick <= BUTTON_LATCH_TTL_MS;
  r1 = g_r1_latch && now - g_r1_tick <= BUTTON_LATCH_TTL_MS;
  triangle = g_triangle_latch &&
             now - g_triangle_tick <= BUTTON_LATCH_TTL_MS;
  g_launch_latch = 0;
  g_l1_latch = 0;
  g_r1_latch = 0;
  g_triangle_latch = 0;
  SDL_UnlockMutex(g_lock);

  g_push_number(state, x);
  g_push_number(state, y);
  g_push_boolean(state, blocked);
  g_push_boolean(state, l1);
  g_push_boolean(state, r1);
  g_push_boolean(state, triangle);
  g_push_boolean(state, launch);
  return 7;
}

int ab_lua_control_install(void) {
  void *target = NULL;
  const uint32_t expected[2] = {0xe92d4010u, 0xe1a04000u};
  const uint32_t expected_push_number[2] = {0xe5902008u, 0xe3a0c003u};
  const uint32_t expected_push_boolean[2] = {0xe5902008u, 0xe2911000u};
  const AbLuaProfile *selected = NULL;
  size_t i;
  nxloader_result result;

  g_lock = SDL_CreateMutex();
  if (!g_lock)
    return 0;

  for (i = 0; i < sizeof(k_lua_profiles) / sizeof(k_lua_profiles[0]); i++) {
    const AbLuaProfile *profile = &k_lua_profiles[i];
    LuaPushNumberFn push_number =
        (LuaPushNumberFn)nxloader_module_vma_to_pointer(
            ab_guest, profile->push_number_vma, 8);
    LuaPushBooleanFn push_boolean =
        (LuaPushBooleanFn)nxloader_module_vma_to_pointer(
            ab_guest, profile->push_boolean_vma, 8);
    void *clock_target = nxloader_module_vma_to_pointer(
        ab_guest, profile->os_clock_vma, 8);

    if (!push_number || !push_boolean || !clock_target)
      continue;
    if (((const uint32_t *)push_number)[0] != expected_push_number[0] ||
        ((const uint32_t *)push_number)[1] != expected_push_number[1] ||
        ((const uint32_t *)push_boolean)[0] != expected_push_boolean[0] ||
        ((const uint32_t *)push_boolean)[1] != expected_push_boolean[1] ||
        ((const uint32_t *)clock_target)[0] != expected[0] ||
        ((const uint32_t *)clock_target)[1] != expected[1])
      continue;

    g_push_number = push_number;
    g_push_boolean = push_boolean;
    target = clock_target;
    selected = profile;
    break;
  }

  if (!selected) {
    ab_log("[control] nenhuma assinatura Lua oficial 8.0.3 corresponde");
    return 0;
  }
  ab_log("[control] perfil Lua %s", selected->name);
  result = nxloader_module_install_hook(ab_guest, (uintptr_t)target,
                                        (uintptr_t)control_rpc, 8);
  ab_log("[control] RPC Lua os.clock: %s", nxloader_result_string(result));
  return result == NXLOADER_OK;
}

void ab_lua_control_set_stick(float x, float y, int blocked) {
  if (!g_lock)
    return;
  SDL_LockMutex(g_lock);
  g_x = x;
  g_y = y;
  g_blocked = blocked;
  SDL_UnlockMutex(g_lock);
}

static void latch(int *value, Uint32 *tick, const char *name) {
  if (!g_lock)
    return;
  SDL_LockMutex(g_lock);
  *value = 1;
  *tick = SDL_GetTicks();
  SDL_UnlockMutex(g_lock);
  ab_log("[control] botao %s", name);
}

void ab_lua_control_press_l1(void) {
  latch(&g_l1_latch, &g_l1_tick, "L1");
}
void ab_lua_control_press_launch(void) {
  latch(&g_launch_latch, &g_launch_tick, "A-disparo");
}
void ab_lua_control_press_r1(void) {
  latch(&g_r1_latch, &g_r1_tick, "R1");
}
void ab_lua_control_press_triangle(void) {
  latch(&g_triangle_latch, &g_triangle_tick, "TRIANGLE");
}

void ab_lua_control_clear_buttons(void) {
  if (!g_lock)
    return;
  SDL_LockMutex(g_lock);
  g_launch_latch = 0;
  g_l1_latch = 0;
  g_r1_latch = 0;
  g_triangle_latch = 0;
  g_launch_tick = g_l1_tick = g_r1_tick = g_triangle_tick = 0;
  SDL_UnlockMutex(g_lock);
}

int ab_lua_control_is_gameplay(void) {
  Uint32 tick = g_gameplay_tick;
  return tick != 0u && SDL_GetTicks() - tick <= 250u;
}
