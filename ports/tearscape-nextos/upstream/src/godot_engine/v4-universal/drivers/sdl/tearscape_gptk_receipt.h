/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef TEARSCAPE_GPTK_RECEIPT_H
#define TEARSCAPE_GPTK_RECEIPT_H

/* TEARSCAPE 0.2.17: the per-run GPTK evidence receipt (nxinput-gptk-event-
 * evidence/1) in the JSON-lines shape the framework's automated on-device
 * controls proof (nx-device-input-proof.py) and the release lock builder
 * (nx-input-proof-lock.py) read back. This unit is PURE: no Godot, no SDL,
 * no nxinput header, so tests/controls/test_tearscape_gptk_receipt.cpp proves
 * the exact line shapes, the vector EDGE semantics and the release
 * attribution on the host. The glue (nxinput_gptk_godot.cpp) only feeds it.
 *
 * Line shapes (byte-exact; the field order is part of the contract):
 *   runtime    {"schema":S,"kind":"runtime","marker":M,"mapping_sha256":H,
 *               "source":"owner|default","gptk_schema":N,"face_layout":F,
 *               "sinks":N}
 *   context    {"schema":S,"kind":"context","context":C,"source":SRC,
 *               "observed":true}
 *   delivery   {"schema":S,"kind":"delivery","context":C,"context_source":SRC,
 *               "control":CTL,"event":"press|motion","decision":"ACTION",
 *               "action":A,"sink":ADAPTER_SINK_ID,"pressed":0|1,
 *               "delivery_count":1}
 *   suppressed {"schema":S,"kind":"suppressed","context":C,
 *               "context_source":SRC,"control":CTL,"event":"press",
 *               "decision":"SUPPRESS","delivery_count":0}
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* --- The adapter sink ids -------------------------------------------- */

/* Semantic GPTK action -> the ADAPTER sink id (nxproject.json
 * controls.actions[].sinks[0]). This table is the single authority for the
 * `sink` field of every receipt/log line; recipes/test_runtime_contract.py
 * asserts it against nxproject.json. */
struct TearsGptkAdapterSink {
	const char *action;
	const char *sink_id;
};

const struct TearsGptkAdapterSink *tears_gptk_adapter_sinks(size_t *p_count);

/* The adapter sink id of an action, or NULL for an unknown action. */
const char *tears_gptk_adapter_sink_id(const char *p_action);

/* --- The receipt file -------------------------------------------------- */

struct TearsGptkReceipt {
	FILE *out;
	bool tried;
};

void tears_gptk_receipt_init(struct TearsGptkReceipt *p_receipt);

/* Open once, append, line-buffered. A NULL/empty path (env absent) silently
 * disables the receipt: every later write is a no-op. Returns whether a file
 * is open after the call. */
bool tears_gptk_receipt_open(struct TearsGptkReceipt *p_receipt, const char *p_path);

/* Append one line (a newline is added). No-op while no file is open. */
void tears_gptk_receipt_write(struct TearsGptkReceipt *p_receipt, const char *p_line);

/* --- The line formatters --------------------------------------------- */
/* Each returns 0 and a NUL-terminated line in p_buffer, or -1 when the line
 * does not fit (p_buffer is then an empty string). NULL strings print as "". */

int tears_gptk_receipt_runtime_line(char *p_buffer, size_t p_size,
		const char *p_schema, const char *p_marker, const char *p_mapping_sha256,
		const char *p_source, unsigned int p_gptk_schema, const char *p_face_layout,
		size_t p_sinks);

int tears_gptk_receipt_context_line(char *p_buffer, size_t p_size,
		const char *p_schema, const char *p_context, const char *p_source);

int tears_gptk_receipt_delivery_line(char *p_buffer, size_t p_size,
		const char *p_schema, const char *p_context, const char *p_context_source,
		const char *p_control, const char *p_event, const char *p_action,
		const char *p_sink, bool p_pressed);

int tears_gptk_receipt_suppressed_line(char *p_buffer, size_t p_size,
		const char *p_schema, const char *p_context, const char *p_context_source,
		const char *p_control);

/* --- Release attribution ----------------------------------------------- */

/* A release may be born from a context change (the runtime releases every
 * latched action outside any feed) and then no control is "current": the
 * release belongs to the control that PRESSED. */
struct TearsGptkPressOwner {
	int control; /* -1: nothing pressed */
};

void tears_gptk_press_owner_init(struct TearsGptkPressOwner *p_owner);

/* Record a delivered edge; returns the control the line must name: the
 * current control on a press (remembered), the remembered control on a
 * release (then forgotten). p_current_control < 0 means "no feeding control". */
int tears_gptk_press_owner_edge(struct TearsGptkPressOwner *p_owner,
		bool p_pressed, int p_current_control);

/* --- Vector EDGE semantics --------------------------------------------- */

/* A vector reaches the sink EVERY frame. Evidence is by EDGE: one line when
 * the vector leaves neutral (pressed=1) and one when it returns (pressed=0)
 * or is released by a context change; never one line per frame. */
struct TearsGptkVectorGesture {
	bool active;
};

enum TearsGptkVectorEdge {
	TEARS_GPTK_VECTOR_EDGE_NONE = 0,
	TEARS_GPTK_VECTOR_EDGE_START = 1,
	TEARS_GPTK_VECTOR_EDGE_STOP = -1,
};

void tears_gptk_vector_gesture_init(struct TearsGptkVectorGesture *p_gesture);

/* Feed one frame: p_deflected is "any direction has strength after the
 * deadzone". Returns START exactly once when a gesture opens, STOP exactly
 * once when it returns to neutral, NONE otherwise. */
int tears_gptk_vector_gesture_feed(struct TearsGptkVectorGesture *p_gesture,
		bool p_deflected);

/* Force-close (release-all / context change): returns true when a gesture
 * was open (one pressed=0 line is due), false when nothing was open. */
bool tears_gptk_vector_gesture_close(struct TearsGptkVectorGesture *p_gesture);

#endif /* TEARSCAPE_GPTK_RECEIPT_H */
