/* SPDX-License-Identifier: GPL-3.0-only */
/* TEARSCAPE 0.2.17: GPTK evidence receipt -- pure unit, see the header. */

#include "tearscape_gptk_receipt.h"

#include <string.h>

/* nxproject.json controls.actions[].sinks[0], one row per declared action.
 * recipes/test_runtime_contract.py fails when this set drifts from nxproject. */
static const struct TearsGptkAdapterSink tears_gptk_adapter_sink_table[] = {
	{ "menu.accept", "engine.ui_accept" },
	{ "menu.back", "engine.ui_cancel" },
	{ "menu.navigate", "engine.ui_direction" },
	{ "player.attack", "engine.input.attack" },
	{ "player.heal", "engine.input.heal" },
	{ "player.move", "engine.input.move" },
	{ "player.open_map", "engine.input.open_map" },
	{ "player.roll", "engine.input.roll" },
	{ "player.select_next", "engine.input.select_next" },
	{ "player.select_previous", "engine.input.select_prev" },
	{ "player.switch_tool", "engine.input.switch_tool" },
	{ "player.use_shield", "engine.input.use_shield" },
	{ "player.use_tool", "engine.input.use_tool" },
	{ "player.zoom_map", "engine.input.zoom_map" },
	{ "system.pause", "engine.input.pause" },
	{ "system.quit", "adapter.system.quit" },
};

const struct TearsGptkAdapterSink *tears_gptk_adapter_sinks(size_t *p_count) {
	if (p_count) {
		*p_count = sizeof(tears_gptk_adapter_sink_table) /
				sizeof(tears_gptk_adapter_sink_table[0]);
	}
	return tears_gptk_adapter_sink_table;
}

const char *tears_gptk_adapter_sink_id(const char *p_action) {
	if (!p_action) {
		return NULL;
	}
	for (const struct TearsGptkAdapterSink &row : tears_gptk_adapter_sink_table) {
		if (strcmp(row.action, p_action) == 0) {
			return row.sink_id;
		}
	}
	return NULL;
}

/* --- file -------------------------------------------------------------- */

void tears_gptk_receipt_init(struct TearsGptkReceipt *p_receipt) {
	if (p_receipt) {
		p_receipt->out = NULL;
		p_receipt->tried = false;
	}
}

bool tears_gptk_receipt_open(struct TearsGptkReceipt *p_receipt, const char *p_path) {
	if (!p_receipt) {
		return false;
	}
	if (p_receipt->out || p_receipt->tried) {
		return p_receipt->out != NULL;
	}
	p_receipt->tried = true;
	if (!p_path || !p_path[0]) {
		return false;
	}
	p_receipt->out = fopen(p_path, "a");
	if (p_receipt->out) {
		setvbuf(p_receipt->out, NULL, _IOLBF, 0);
	}
	return p_receipt->out != NULL;
}

void tears_gptk_receipt_write(struct TearsGptkReceipt *p_receipt, const char *p_line) {
	if (!p_receipt || !p_receipt->out || !p_line) {
		return;
	}
	fputs(p_line, p_receipt->out);
	fputc('\n', p_receipt->out);
}

/* --- formatters -------------------------------------------------------- */

static const char *tears_gptk_str(const char *p_value) {
	return p_value ? p_value : "";
}

static int tears_gptk_finish(char *p_buffer, size_t p_size, int p_written) {
	if (p_written < 0 || (size_t)p_written >= p_size) {
		if (p_size > 0) {
			p_buffer[0] = '\0';
		}
		return -1;
	}
	return 0;
}

int tears_gptk_receipt_runtime_line(char *p_buffer, size_t p_size,
		const char *p_schema, const char *p_marker, const char *p_mapping_sha256,
		const char *p_source, unsigned int p_gptk_schema, const char *p_face_layout,
		size_t p_sinks) {
	if (!p_buffer || p_size == 0) {
		return -1;
	}
	const int written = snprintf(p_buffer, p_size,
			"{\"schema\":\"%s\",\"kind\":\"runtime\",\"marker\":\"%s\","
			"\"mapping_sha256\":\"%s\",\"source\":\"%s\",\"gptk_schema\":%u,"
			"\"face_layout\":\"%s\",\"sinks\":%zu}",
			tears_gptk_str(p_schema), tears_gptk_str(p_marker),
			tears_gptk_str(p_mapping_sha256), tears_gptk_str(p_source),
			p_gptk_schema, tears_gptk_str(p_face_layout), p_sinks);
	return tears_gptk_finish(p_buffer, p_size, written);
}

int tears_gptk_receipt_context_line(char *p_buffer, size_t p_size,
		const char *p_schema, const char *p_context, const char *p_source) {
	if (!p_buffer || p_size == 0) {
		return -1;
	}
	const int written = snprintf(p_buffer, p_size,
			"{\"schema\":\"%s\",\"kind\":\"context\",\"context\":\"%s\","
			"\"source\":\"%s\",\"observed\":true}",
			tears_gptk_str(p_schema), tears_gptk_str(p_context),
			tears_gptk_str(p_source));
	return tears_gptk_finish(p_buffer, p_size, written);
}

int tears_gptk_receipt_delivery_line(char *p_buffer, size_t p_size,
		const char *p_schema, const char *p_context, const char *p_context_source,
		const char *p_control, const char *p_event, const char *p_action,
		const char *p_sink, bool p_pressed) {
	if (!p_buffer || p_size == 0) {
		return -1;
	}
	const int written = snprintf(p_buffer, p_size,
			"{\"schema\":\"%s\",\"kind\":\"delivery\",\"context\":\"%s\","
			"\"context_source\":\"%s\",\"control\":\"%s\",\"event\":\"%s\","
			"\"decision\":\"ACTION\",\"action\":\"%s\",\"sink\":\"%s\","
			"\"pressed\":%d,\"delivery_count\":1}",
			tears_gptk_str(p_schema), tears_gptk_str(p_context),
			tears_gptk_str(p_context_source), tears_gptk_str(p_control),
			tears_gptk_str(p_event), tears_gptk_str(p_action),
			tears_gptk_str(p_sink), p_pressed ? 1 : 0);
	return tears_gptk_finish(p_buffer, p_size, written);
}

int tears_gptk_receipt_suppressed_line(char *p_buffer, size_t p_size,
		const char *p_schema, const char *p_context, const char *p_context_source,
		const char *p_control) {
	if (!p_buffer || p_size == 0) {
		return -1;
	}
	const int written = snprintf(p_buffer, p_size,
			"{\"schema\":\"%s\",\"kind\":\"suppressed\",\"context\":\"%s\","
			"\"context_source\":\"%s\",\"control\":\"%s\",\"event\":\"press\","
			"\"decision\":\"SUPPRESS\",\"delivery_count\":0}",
			tears_gptk_str(p_schema), tears_gptk_str(p_context),
			tears_gptk_str(p_context_source), tears_gptk_str(p_control));
	return tears_gptk_finish(p_buffer, p_size, written);
}

/* --- release attribution ------------------------------------------------ */

void tears_gptk_press_owner_init(struct TearsGptkPressOwner *p_owner) {
	if (p_owner) {
		p_owner->control = -1;
	}
}

int tears_gptk_press_owner_edge(struct TearsGptkPressOwner *p_owner,
		bool p_pressed, int p_current_control) {
	if (!p_owner) {
		return p_current_control;
	}
	if (p_pressed) {
		p_owner->control = p_current_control;
		return p_current_control;
	}
	const int owner = p_owner->control;
	p_owner->control = -1;
	return owner;
}

/* --- vector EDGE ----------------------------------------------------------- */

void tears_gptk_vector_gesture_init(struct TearsGptkVectorGesture *p_gesture) {
	if (p_gesture) {
		p_gesture->active = false;
	}
}

int tears_gptk_vector_gesture_feed(struct TearsGptkVectorGesture *p_gesture,
		bool p_deflected) {
	if (!p_gesture) {
		return TEARS_GPTK_VECTOR_EDGE_NONE;
	}
	if (!p_gesture->active && p_deflected) {
		p_gesture->active = true;
		return TEARS_GPTK_VECTOR_EDGE_START;
	}
	if (p_gesture->active && !p_deflected) {
		p_gesture->active = false;
		return TEARS_GPTK_VECTOR_EDGE_STOP;
	}
	return TEARS_GPTK_VECTOR_EDGE_NONE;
}

bool tears_gptk_vector_gesture_close(struct TearsGptkVectorGesture *p_gesture) {
	if (!p_gesture || !p_gesture->active) {
		return false;
	}
	p_gesture->active = false;
	return true;
}
