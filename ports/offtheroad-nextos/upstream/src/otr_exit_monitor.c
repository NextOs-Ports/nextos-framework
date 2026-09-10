/* SPDX-License-Identifier: GPL-3.0-only */
#include "otr_exit_monitor.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void monitor_sleep(unsigned interval_us) {
  struct timespec requested;
  struct timespec remaining;
  requested.tv_sec = (time_t)(interval_us / 1000000u);
  requested.tv_nsec = (long)(interval_us % 1000000u) * 1000L;
  while (nanosleep(&requested, &remaining) != 0 && errno == EINTR)
    requested = remaining;
}

static void deliver_once(otr_exit_monitor *monitor, otr_exit_source source) {
  if (atomic_exchange_explicit(&monitor->delivered, true,
                               memory_order_acq_rel))
    return;
  (void)nxinput_exit_chord_consume(&monitor->chord);
  atomic_store_explicit(&monitor->delivered_source, (int)source,
                        memory_order_release);
  atomic_fetch_add_explicit(&monitor->delivery_count, 1u,
                            memory_order_relaxed);
  monitor->deliver(monitor->user, source);
}

static void *monitor_thread(void *opaque) {
  otr_exit_monitor *monitor = (otr_exit_monitor *)opaque;
  while (!atomic_load_explicit(&monitor->stop_requested,
                               memory_order_acquire)) {
    otr_exit_sample sample;
    int primary_fired = 0;
    memset(&sample, 0, sizeof(sample));
    if (atomic_exchange_explicit(&monitor->reset_requested, false,
                                 memory_order_acq_rel))
      nxinput_exit_chord_reset_hold(&monitor->chord);
    if (monitor->sample(monitor->user, &sample) == 0) {
      atomic_fetch_add_explicit(&monitor->poll_count, 1u,
                                memory_order_relaxed);
      if (sample.primary_authoritative) {
        primary_fired = nxinput_exit_chord_update(
            &monitor->chord, sample.select_down, sample.start_down);
      } else {
        /* Switching authority cannot inherit half of an SDL hold. The evdev
         * adapter has already edge-qualified fallback_fired. */
        (void)nxinput_exit_chord_update(&monitor->chord, 0, 0);
      }
      if (primary_fired)
        deliver_once(monitor, OTR_EXIT_SOURCE_SDL_PRIMARY);
      else if (!sample.primary_authoritative && sample.fallback_fired)
        deliver_once(monitor, OTR_EXIT_SOURCE_EVDEV_FALLBACK);
    }
    monitor_sleep(monitor->interval_us);
  }
  return NULL;
}

int otr_exit_monitor_start(otr_exit_monitor *monitor,
                           otr_exit_sample_fn sample,
                           otr_exit_deliver_fn deliver, void *user,
                           unsigned interval_us) {
  if (monitor == NULL || sample == NULL || deliver == NULL) return -1;
  memset(monitor, 0, sizeof(*monitor));
  nxinput_exit_chord_init(&monitor->chord, 0u);
  monitor->sample = sample;
  monitor->deliver = deliver;
  monitor->user = user;
  monitor->interval_us = interval_us == 0u ? 16000u : interval_us;
  atomic_init(&monitor->stop_requested, false);
  atomic_init(&monitor->reset_requested, false);
  atomic_init(&monitor->started, false);
  atomic_init(&monitor->delivered, false);
  atomic_init(&monitor->poll_count, 0u);
  atomic_init(&monitor->delivery_count, 0u);
  atomic_init(&monitor->delivered_source, (int)OTR_EXIT_SOURCE_NONE);
  if (pthread_create(&monitor->thread, NULL, monitor_thread, monitor) != 0)
    return -1;
  atomic_store_explicit(&monitor->started, true, memory_order_release);
  return 0;
}

void otr_exit_monitor_reset_hold(otr_exit_monitor *monitor) {
  if (monitor != NULL &&
      atomic_load_explicit(&monitor->started, memory_order_acquire))
    atomic_store_explicit(&monitor->reset_requested, true,
                          memory_order_release);
}

void otr_exit_monitor_stop(otr_exit_monitor *monitor) {
  if (monitor == NULL ||
      !atomic_exchange_explicit(&monitor->started, false,
                                memory_order_acq_rel))
    return;
  atomic_store_explicit(&monitor->stop_requested, true, memory_order_release);
  (void)pthread_join(monitor->thread, NULL);
}

unsigned otr_exit_monitor_poll_count(const otr_exit_monitor *monitor) {
  return monitor == NULL
             ? 0u
             : atomic_load_explicit(&monitor->poll_count,
                                    memory_order_relaxed);
}

unsigned otr_exit_monitor_delivery_count(const otr_exit_monitor *monitor) {
  return monitor == NULL
             ? 0u
             : atomic_load_explicit(&monitor->delivery_count,
                                    memory_order_relaxed);
}

otr_exit_source otr_exit_monitor_source(const otr_exit_monitor *monitor) {
  return monitor == NULL
             ? OTR_EXIT_SOURCE_NONE
             : (otr_exit_source)atomic_load_explicit(
                   &monitor->delivered_source, memory_order_acquire);
}

int otr_exit_monitor_format_receipt(const otr_exit_monitor *monitor,
                                    char *line, size_t line_size) {
  const char *source;
  int written;
  if (line != NULL && line_size != 0u) line[0] = '\0';
  if (monitor == NULL || line == NULL || line_size == 0u) return -1;
  switch (otr_exit_monitor_source(monitor)) {
    case OTR_EXIT_SOURCE_SDL_PRIMARY:
      source = "sdl-primary";
      break;
    case OTR_EXIT_SOURCE_EVDEV_FALLBACK:
      source = "evdev-fallback";
      break;
    default:
      source = "not-observed";
      break;
  }
  written = snprintf(
      line, line_size,
      "INPUT-EXIT-RECEIPT: schema=offtheroad-exit/1 source=%s "
      "delivery_count=%u polls=%u independent_guest_loop=1 sticky=1",
      source, otr_exit_monitor_delivery_count(monitor),
      otr_exit_monitor_poll_count(monitor));
  if (written < 0 || (size_t)written >= line_size) {
    line[0] = '\0';
    return -1;
  }
  return 0;
}
