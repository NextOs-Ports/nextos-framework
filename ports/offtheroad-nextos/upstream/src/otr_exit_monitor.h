/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef OTR_EXIT_MONITOR_H
#define OTR_EXIT_MONITOR_H

#include "nxinput_exit_chord.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>

typedef enum otr_exit_source {
  OTR_EXIT_SOURCE_NONE = 0,
  OTR_EXIT_SOURCE_SDL_PRIMARY = 1,
  OTR_EXIT_SOURCE_EVDEV_FALLBACK = 2,
} otr_exit_source;

typedef struct otr_exit_sample {
  int primary_authoritative;
  int select_down;
  int start_down;
  int fallback_fired;
} otr_exit_sample;

typedef int (*otr_exit_sample_fn)(void *user, otr_exit_sample *sample);
typedef void (*otr_exit_deliver_fn)(void *user, otr_exit_source source);

typedef struct otr_exit_monitor {
  nxinput_exit_chord chord;
  pthread_t thread;
  otr_exit_sample_fn sample;
  otr_exit_deliver_fn deliver;
  void *user;
  unsigned interval_us;
  atomic_bool stop_requested;
  atomic_bool reset_requested;
  atomic_bool started;
  atomic_bool delivered;
  atomic_uint poll_count;
  atomic_uint delivery_count;
  atomic_int delivered_source;
} otr_exit_monitor;

/* Host-owned poller: it never calls the guest input or render loop. The
 * primary sample is normalized through nxinput's sticky SELECT+START state
 * machine. An adapter may supply an already edge-qualified evdev fallback,
 * but only while primary_authoritative is false. */
int otr_exit_monitor_start(otr_exit_monitor *monitor,
                           otr_exit_sample_fn sample,
                           otr_exit_deliver_fn deliver, void *user,
                           unsigned interval_us);

/* Hot-unplug or primary replacement drops an incomplete hold. A delivered
 * request remains sticky and can never be delivered twice. */
void otr_exit_monitor_reset_hold(otr_exit_monitor *monitor);
void otr_exit_monitor_stop(otr_exit_monitor *monitor);
unsigned otr_exit_monitor_poll_count(const otr_exit_monitor *monitor);
unsigned otr_exit_monitor_delivery_count(const otr_exit_monitor *monitor);
otr_exit_source otr_exit_monitor_source(const otr_exit_monitor *monitor);
int otr_exit_monitor_format_receipt(const otr_exit_monitor *monitor,
                                    char *line, size_t line_size);

#endif /* OTR_EXIT_MONITOR_H */
