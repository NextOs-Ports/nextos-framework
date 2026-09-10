/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "otr_gptk.h"

#include "nxinput_gptk.h"
#include "nxinput_gptk_loader.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct otr_gptk_state {
  nxinput_gptk map;
  nxinput_gptk default_map;
  nxinput_gptk_dispatcher dispatcher;
  nxinput_gptk_source_guard source_guard;
  nxinput_gptk_load_receipt load_receipt;
  otr_gptk_hooks hooks;
  int ready;
  int gameplay;
  unsigned long physical_edges;
  unsigned long physical_vectors;
  unsigned long semantic_deliveries;
  unsigned long duplicate_deliveries;
  unsigned long deduplicated_edges;
  unsigned long suppressed_raw_paths;
  unsigned long context_switches;
  unsigned long mapped_delivery_checks;
  unsigned long delivery_contract_failures;
  unsigned int deliveries_in_feed;
  unsigned int maximum_deliveries_in_feed;
  int tracking_physical_feed;
  int ab_swap_configured;
  int swapped_a_observed;
  int swapped_b_observed;
  int start_select_observed;
  unsigned int terminal_delivery_count;
  unsigned int terminal_poll_count;
  int terminal_independent_guest_loop;
  char terminal_source[32];
  char last_action_in_feed[NXINPUT_GPTK_ACTION_MAX + 1u];
} otr_gptk_state;

static otr_gptk_state g_gptk;

static const char *const k_actions[] = {
    "otr.accelerate", "otr.brake", "otr.confirm", "otr.back",
    "otr.pause",      "cursor.click", "camera.steer", "camera.look",
    "cursor.move",
};

static void set_error(char *error, size_t cap, const char *fmt, ...) {
  va_list ap;
  if (error == NULL || cap == 0u) return;
  va_start(ap, fmt);
  (void)vsnprintf(error, cap, fmt, ap);
  va_end(ap);
}

static void digital_sink(void *user, const char *action, int pressed,
                         float value) {
  otr_gptk_state *state = (otr_gptk_state *)user;
  state->deliveries_in_feed++;
  state->semantic_deliveries++;
  if (state->tracking_physical_feed) {
    if (strcmp(state->last_action_in_feed, action) == 0)
      state->duplicate_deliveries++;
    else
      (void)snprintf(state->last_action_in_feed,
                     sizeof(state->last_action_in_feed), "%s", action);
  }
  if (state->hooks.digital != NULL)
    state->hooks.digital(action, pressed, value);
}

static void vector_sink(void *user, const char *action, float x, float y) {
  otr_gptk_state *state = (otr_gptk_state *)user;
  state->deliveries_in_feed++;
  state->semantic_deliveries++;
  if (state->tracking_physical_feed) {
    if (strcmp(state->last_action_in_feed, action) == 0)
      state->duplicate_deliveries++;
    else
      (void)snprintf(state->last_action_in_feed,
                     sizeof(state->last_action_in_feed), "%s", action);
  }
  if (state->hooks.vector != NULL) state->hooks.vector(action, x, y);
}

static int context_ab_is_swapped(nxinput_gptk_context context) {
  const char *selected_a = nxinput_gptk_action(&g_gptk.map, context,
                                                NXINPUT_GPTK_A);
  const char *selected_b = nxinput_gptk_action(&g_gptk.map, context,
                                                NXINPUT_GPTK_B);
  const char *default_a = nxinput_gptk_action(&g_gptk.default_map, context,
                                               NXINPUT_GPTK_A);
  const char *default_b = nxinput_gptk_action(&g_gptk.default_map, context,
                                               NXINPUT_GPTK_B);
  return selected_a != NULL && selected_b != NULL && default_a != NULL &&
         default_b != NULL && strcmp(selected_a, default_b) == 0 &&
         strcmp(selected_b, default_a) == 0;
}

static void record_delivery_contract(int mapped, int state_transition) {
  if (!mapped || !state_transition) return;
  g_gptk.mapped_delivery_checks++;
  if (g_gptk.deliveries_in_feed != 1u)
    g_gptk.delivery_contract_failures++;
  if (g_gptk.deliveries_in_feed > g_gptk.maximum_deliveries_in_feed)
    g_gptk.maximum_deliveries_in_feed = g_gptk.deliveries_in_feed;
}

int otr_gptk_init(const char *game_dir, int drawable_w, int drawable_h,
                  const otr_gptk_hooks *hooks, char *error,
                  size_t error_size) {
  int owner_fd = -1;
  int defaults_fd = -1;
  nxinput_gptk_load_receipt default_receipt;
  size_t i;
  int rc;

  memset(&g_gptk, 0, sizeof(g_gptk));
  if (hooks == NULL || hooks->digital == NULL || hooks->vector == NULL) {
    set_error(error, error_size, "NXI1002: incomplete game sink table");
    return -1;
  }
  if (game_dir == NULL || game_dir[0] == '\0') {
    set_error(error, error_size, "NXI1002: invalid game directory");
    return -1;
  }
  owner_fd = open(game_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (owner_fd >= 0)
    defaults_fd = openat(owner_fd, "defaults",
                         O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (owner_fd < 0 || defaults_fd < 0) {
    set_error(error, error_size,
              "NXI1002: owner/default mapping directory unavailable");
    if (defaults_fd >= 0) (void)close(defaults_fd);
    if (owner_fd >= 0) (void)close(owner_fd);
    return -1;
  }
  rc = nxinput_gptk_load_at(
      defaults_fd, defaults_fd, k_actions,
      sizeof(k_actions) / sizeof(k_actions[0]), &g_gptk.default_map,
      &default_receipt);
  if (rc == 0)
    rc = nxinput_gptk_load_at(
      owner_fd, defaults_fd, k_actions,
      sizeof(k_actions) / sizeof(k_actions[0]), &g_gptk.map,
      &g_gptk.load_receipt);
  (void)close(defaults_fd);
  (void)close(owner_fd);
  if (rc != 0) {
    set_error(error, error_size,
              "NXI%d: neither owner nor immutable default GPTK is valid", rc);
    return -1;
  }
  if (g_gptk.map.port[0] != '\0' && strcmp(g_gptk.map.port, "offtheroad") != 0) {
    set_error(error, error_size, "NXI1001: mapping belongs to port %s",
              g_gptk.map.port);
    return -1;
  }
  g_gptk.ab_swap_configured =
      g_gptk.load_receipt.source == NXINPUT_GPTK_LOAD_OWNER &&
      context_ab_is_swapped(NXINPUT_GPTK_CONTEXT_MENU) &&
      context_ab_is_swapped(NXINPUT_GPTK_CONTEXT_GAMEPLAY) &&
      context_ab_is_swapped(NXINPUT_GPTK_CONTEXT_CURSOR);

  g_gptk.hooks = *hooks;
  nxinput_gptk_dispatcher_init(&g_gptk.dispatcher, &g_gptk.map);
  if (nxinput_gptk_dispatcher_configure_motion(
          &g_gptk.dispatcher, drawable_w, drawable_h) != 0) {
    set_error(error, error_size, "NXI1002: invalid drawable for GPTK motion");
    return -1;
  }
  for (i = 0u; i < sizeof(k_actions) / sizeof(k_actions[0]); i++) {
    const char *action = k_actions[i];
    if (strcmp(action, "cursor.move") == 0 ||
        strcmp(action, "camera.steer") == 0 ||
        strcmp(action, "camera.look") == 0) {
      if (nxinput_gptk_dispatcher_register_vector(
              &g_gptk.dispatcher, action, vector_sink, &g_gptk) != 0) {
        set_error(error, error_size, "NXI1002: vector sink table full");
        return -1;
      }
    } else if (nxinput_gptk_dispatcher_register(
                   &g_gptk.dispatcher, action, digital_sink, &g_gptk) != 0) {
      set_error(error, error_size, "NXI1002: digital sink table full");
      return -1;
    }
  }
  /* Menus are touch-driven.  Cursor is the initial live context; gameplay is
   * selected only after cMulti reports a real local player. */
  nxinput_gptk_dispatcher_set_context(&g_gptk.dispatcher,
                                      NXINPUT_GPTK_CONTEXT_CURSOR);
  nxinput_gptk_source_guard_init(&g_gptk.source_guard,
                                 &g_gptk.dispatcher);
  g_gptk.gameplay = 0;
  g_gptk.ready = 1;
  if (error != NULL && error_size > 0u) error[0] = '\0';
  return 0;
}

int otr_gptk_ready(void) { return g_gptk.ready; }

void otr_gptk_set_gameplay(int gameplay) {
  nxinput_gptk_context next;
  uint32_t primary_mask;
  if (!g_gptk.ready) return;
  gameplay = gameplay != 0;
  if (gameplay == g_gptk.gameplay) return;
  next = gameplay ? NXINPUT_GPTK_CONTEXT_GAMEPLAY
                  : NXINPUT_GPTK_CONTEXT_CURSOR;
  g_gptk.deliveries_in_feed = 0u;
  primary_mask = nxinput_gptk_dispatcher_primary_mask(&g_gptk.source_guard);
  nxinput_gptk_source_guard_reset(&g_gptk.dispatcher,
                                  &g_gptk.source_guard);
  nxinput_gptk_dispatcher_set_context(&g_gptk.dispatcher, next);
  nxinput_gptk_dispatcher_set_primary_mask(
      &g_gptk.dispatcher, &g_gptk.source_guard, primary_mask);
  g_gptk.gameplay = gameplay;
  g_gptk.context_switches++;
}

void otr_gptk_set_primary_mask(unsigned int control_mask) {
  if (!g_gptk.ready) return;
  nxinput_gptk_dispatcher_set_primary_mask(
      &g_gptk.dispatcher, &g_gptk.source_guard, (uint32_t)control_mask);
}

int otr_gptk_button_mapped(int control) {
  if (!g_gptk.ready || control < 0 ||
      control >= (int)NXINPUT_GPTK_CONTROL_COUNT)
    return 0;
  return nxinput_gptk_action(&g_gptk.map, g_gptk.dispatcher.context, control) !=
         NULL;
}

int otr_gptk_stick_owned(int control) {
  if (!g_gptk.ready) return 0;
  return nxinput_gptk_dispatcher_control_suppressed(&g_gptk.dispatcher,
                                                     control);
}

void otr_gptk_feed_button(int control, int pressed, float value) {
  uint32_t bit;
  int already;
  int mapped;
  int transition;
  if (!g_gptk.ready || control < 0 ||
      control >= (int)NXINPUT_GPTK_CONTROL_COUNT)
    return;
  bit = UINT32_C(1) << (unsigned)control;
  already = (g_gptk.source_guard.source_down[NXINPUT_GPTK_SOURCE_PRIMARY] &
             bit) != 0u;
  mapped = otr_gptk_button_mapped(control);
  transition = (pressed != 0) != already;
  if ((pressed && already) || (!pressed && !already))
    g_gptk.deduplicated_edges++;
  g_gptk.physical_edges++;
  g_gptk.deliveries_in_feed = 0u;
  g_gptk.last_action_in_feed[0] = '\0';
  g_gptk.tracking_physical_feed = 1;
  nxinput_gptk_dispatcher_feed_source(
      &g_gptk.dispatcher, &g_gptk.source_guard,
      NXINPUT_GPTK_SOURCE_PRIMARY, control, pressed, value);
  g_gptk.tracking_physical_feed = 0;
  record_delivery_contract(mapped, transition);
  if (g_gptk.ab_swap_configured && pressed && transition &&
      g_gptk.deliveries_in_feed == 1u) {
    if (control == NXINPUT_GPTK_A) g_gptk.swapped_a_observed = 1;
    if (control == NXINPUT_GPTK_B) g_gptk.swapped_b_observed = 1;
  }
  if (mapped) g_gptk.suppressed_raw_paths++;
}

void otr_gptk_feed_stick(int control, float x, float y, float dt_seconds) {
  int mapped;
  if (!g_gptk.ready) return;
  mapped = otr_gptk_stick_owned(control);
  g_gptk.physical_vectors++;
  g_gptk.deliveries_in_feed = 0u;
  g_gptk.last_action_in_feed[0] = '\0';
  g_gptk.tracking_physical_feed = 1;
  nxinput_gptk_dispatcher_feed_stick(&g_gptk.dispatcher, control, x, y,
                                     dt_seconds);
  g_gptk.tracking_physical_feed = 0;
  record_delivery_contract(mapped, 1);
  if (mapped) g_gptk.suppressed_raw_paths++;
}

void otr_gptk_note_terminal_chord(void) {
  if (g_gptk.ready) g_gptk.start_select_observed = 1;
}

void otr_gptk_note_terminal_receipt(const char *source,
                                    unsigned int delivery_count,
                                    unsigned int poll_count,
                                    int independent_guest_loop) {
  if (!g_gptk.ready || source == NULL || source[0] == '\0') return;
  g_gptk.start_select_observed = delivery_count == 1u;
  g_gptk.terminal_delivery_count = delivery_count;
  g_gptk.terminal_poll_count = poll_count;
  g_gptk.terminal_independent_guest_loop = independent_guest_loop != 0;
  (void)snprintf(g_gptk.terminal_source, sizeof(g_gptk.terminal_source),
                 "%s", source);
}

void otr_gptk_release_all(void) {
  if (!g_gptk.ready) return;
  nxinput_gptk_source_guard_reset(&g_gptk.dispatcher,
                                  &g_gptk.source_guard);
}

const char *otr_gptk_context_name(void) {
  if (!g_gptk.ready) return "unavailable";
  return g_gptk.gameplay ? "gameplay" : "cursor";
}

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

int otr_gptk_write_receipt(const char *path) {
  char temp[1024];
  char json[1536];
  int fd;
  int n;
  int delivery_count;
  int double_input;
  if (!g_gptk.ready || path == NULL || path[0] == '\0' ||
      strlen(path) > sizeof(temp) - 48u)
    return -1;
  delivery_count = g_gptk.mapped_delivery_checks > 0u &&
                           g_gptk.delivery_contract_failures == 0u
                       ? 1
                       : 0;
  double_input = g_gptk.duplicate_deliveries > 0u ||
                 g_gptk.maximum_deliveries_in_feed > 1u;
  n = snprintf(
      json, sizeof(json),
      "{\"schema\":\"org.nextos.input-evidence\",\"schema_version\":1,"
      "\"port_id\":\"offtheroad\",\"mapping_port\":\"%s\","
      "\"mapping_source\":\"%s\",\"gptk_sha256\":\"%s\","
      "\"parser\":\"ok\",\"dispatcher\":\"ok\",\"context\":\"%s\","
      "\"sink_count\":%lu,\"physical_edges\":%lu,"
      "\"physical_vectors\":%lu,\"semantic_deliveries\":%lu,"
      "\"delivery_count\":%d,\"double_input\":%s,"
      "\"ab_swap_configured\":%s,\"ab_swap_observed\":%s,"
      "\"start_select_observed\":%s,"
      "\"terminal_source\":\"%s\",\"terminal_delivery_count\":%u,"
      "\"terminal_polls\":%u,\"terminal_independent_guest_loop\":%s,"
      "\"duplicate_deliveries\":%lu,\"deduplicated_edges\":%lu,"
      "\"raw_paths_suppressed\":%lu,\"context_switches\":%lu}\n",
      g_gptk.map.port[0] ? g_gptk.map.port : "offtheroad",
      nxinput_gptk_load_source_name(
          (nxinput_gptk_load_source)g_gptk.load_receipt.source),
      g_gptk.load_receipt.selected_sha256,
      otr_gptk_context_name(),
      (unsigned long)(g_gptk.dispatcher.sink_count +
                      g_gptk.dispatcher.vector_sink_count),
      g_gptk.physical_edges, g_gptk.physical_vectors,
      g_gptk.semantic_deliveries, delivery_count,
      double_input ? "true" : "false",
      g_gptk.ab_swap_configured ? "true" : "false",
      (g_gptk.swapped_a_observed && g_gptk.swapped_b_observed) ? "true"
                                                                : "false",
      g_gptk.start_select_observed ? "true" : "false",
      g_gptk.terminal_source[0] ? g_gptk.terminal_source : "not-observed",
      g_gptk.terminal_delivery_count, g_gptk.terminal_poll_count,
      g_gptk.terminal_independent_guest_loop ? "true" : "false",
      g_gptk.duplicate_deliveries,
      g_gptk.deduplicated_edges, g_gptk.suppressed_raw_paths,
      g_gptk.context_switches);
  if (n < 0 || (size_t)n >= sizeof(json)) return -1;
  n = snprintf(temp, sizeof(temp), "%s.tmp.%ld", path, (long)getpid());
  if (n < 0 || (size_t)n >= sizeof(temp)) return -1;
  (void)unlink(temp);
  fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) return -1;
  if (fchmod(fd, 0600) != 0 || write_all(fd, json, strlen(json)) != 0 ||
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

int otr_gptk_write_load_receipt(const char *path) {
  char json[2048];
  char temp[1024];
  int fd;
  int n;
  if (!g_gptk.ready || path == NULL || path[0] == '\0' ||
      strlen(path) > sizeof(temp) - 48u ||
      nxinput_gptk_load_receipt_json(&g_gptk.load_receipt, json,
                                     sizeof(json)) != 0)
    return -1;
  n = snprintf(temp, sizeof(temp), "%s.tmp.%ld", path, (long)getpid());
  if (n < 0 || (size_t)n >= sizeof(temp)) return -1;
  (void)unlink(temp);
  fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) return -1;
  if (fchmod(fd, 0600) != 0 || write_all(fd, json, strlen(json)) != 0 ||
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
