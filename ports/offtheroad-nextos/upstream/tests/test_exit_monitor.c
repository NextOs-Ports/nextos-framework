/* SPDX-License-Identifier: GPL-3.0-only */
#include "otr_exit_monitor.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct probe {
  int primary;
  atomic_uint samples;
  atomic_uint deliveries;
  atomic_int source;
} probe;

static void require(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "test_exit_monitor: %s\n", message);
    exit(1);
  }
}

static int sample(void *opaque, otr_exit_sample *state) {
  probe *value = (probe *)opaque;
  atomic_fetch_add_explicit(&value->samples, 1u, memory_order_relaxed);
  memset(state, 0, sizeof(*state));
  state->primary_authoritative = value->primary;
  if (value->primary) {
    state->select_down = 1;
    state->start_down = 1;
  } else {
    state->fallback_fired = 1;
  }
  return 0;
}

static void deliver(void *opaque, otr_exit_source source) {
  probe *value = (probe *)opaque;
  atomic_fetch_add_explicit(&value->deliveries, 1u, memory_order_relaxed);
  atomic_store_explicit(&value->source, (int)source, memory_order_release);
}

static int wait_for_delivery(const probe *value) {
  struct timespec interval = {0, 1000000L};
  unsigned i;
  for (i = 0u; i < 1000u; i++) {
    if (atomic_load_explicit(&value->deliveries, memory_order_acquire) != 0u)
      return 0;
    (void)nanosleep(&interval, NULL);
  }
  return -1;
}

static void initialize_probe(probe *value, int primary) {
  memset(value, 0, sizeof(*value));
  value->primary = primary;
  atomic_init(&value->samples, 0u);
  atomic_init(&value->deliveries, 0u);
  atomic_init(&value->source, (int)OTR_EXIT_SOURCE_NONE);
}

int main(void) {
  otr_exit_monitor monitor;
  probe primary;
  probe fallback;
  char receipt[256];
  struct timespec settle = {0, 20000000L};

  /* There is deliberately no pump_input or guest/render call in this test. */
  initialize_probe(&primary, 1);
  require(otr_exit_monitor_start(&monitor, sample, deliver, &primary, 1000u) ==
              0,
          "primary monitor did not start");
  require(wait_for_delivery(&primary) == 0,
          "SDL-primary chord depended on a guest loop");
  (void)nanosleep(&settle, NULL);
  otr_exit_monitor_stop(&monitor);
  require(atomic_load(&primary.deliveries) == 1u &&
              otr_exit_monitor_delivery_count(&monitor) == 1u &&
              atomic_load(&primary.source) == OTR_EXIT_SOURCE_SDL_PRIMARY,
          "primary hold was not sticky/exactly-once");
  require(otr_exit_monitor_format_receipt(&monitor, receipt,
                                          sizeof(receipt)) == 0 &&
              strstr(receipt, "source=sdl-primary") != NULL &&
              strstr(receipt, "delivery_count=1") != NULL &&
              strstr(receipt, "independent_guest_loop=1") != NULL,
          "primary receipt is incomplete");

  initialize_probe(&fallback, 0);
  require(otr_exit_monitor_start(&monitor, sample, deliver, &fallback, 1000u) ==
              0,
          "fallback monitor did not start");
  require(wait_for_delivery(&fallback) == 0,
          "evdev fallback depended on a guest loop");
  (void)nanosleep(&settle, NULL);
  otr_exit_monitor_stop(&monitor);
  require(atomic_load(&fallback.deliveries) == 1u &&
              otr_exit_monitor_delivery_count(&monitor) == 1u &&
              atomic_load(&fallback.source) ==
                  OTR_EXIT_SOURCE_EVDEV_FALLBACK,
          "fallback edge was not sticky/exactly-once");
  require(otr_exit_monitor_format_receipt(&monitor, receipt,
                                          sizeof(receipt)) == 0 &&
              strstr(receipt, "source=evdev-fallback") != NULL,
          "fallback receipt is incomplete");

  puts("test_exit_monitor: PASS");
  return 0;
}
