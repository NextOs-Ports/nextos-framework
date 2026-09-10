/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef TITANSOULS_FRAMEWORK_BRIDGE_H
#define TITANSOULS_FRAMEWORK_BRIDGE_H

#include <SDL2/SDL.h>

#include "nxgl.h"
#include "nxinput.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Probe and apply only capability-gated, process-local framework policy for
 * the ARMv7 runtime.  The caller must invoke this before SDL, EGL, audio or
 * controller initialization. */
int ts_framework_preflight(const char *game_dir);

/* Publish authoritative evidence captured by a successful nxgl API-v2 open.
 * A free-form/legacy graphics report is deliberately not accepted. */
int ts_framework_publish_graphics(const nxgl_report_v2 *report);

/* Publish a live nxinput context.  nxinput remains owned by the adapter. */
int ts_framework_publish_input(const nxinput_context *input);

/* Classify the real SDL backend used by the OpenSL/FMOD Ex adapter, validate
 * Titan Souls' explicit nxaudio contract, and publish the opened device. */
int ts_framework_publish_audio(SDL_AudioDeviceID device,
                               const SDL_AudioSpec *actual);

/* Final fail-closed capability gate.  Each successful input/graphics/audio
 * publication invokes it automatically when it completes the receipt trio,
 * regardless of arrival order.  This explicit API is an idempotent query;
 * it returns failure while any receipt is still pending. */
int ts_framework_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* TITANSOULS_FRAMEWORK_BRIDGE_H */
