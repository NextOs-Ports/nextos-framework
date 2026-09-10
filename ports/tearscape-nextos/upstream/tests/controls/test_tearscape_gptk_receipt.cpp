/* SPDX-License-Identifier: GPL-3.0-only */
/* Host gate for the GPTK evidence receipt (tearscape_gptk_receipt.{h,cpp}):
 * the byte-exact nxinput-gptk-event-evidence/1 JSON line shapes the
 * framework's automated on-device proof and the release lock read back, the
 * adapter sink-id table, the append/line-buffered receipt file (silently off
 * without NXGPTK_RECEIPT), release attribution to the pressing control and
 * the vector EDGE semantics (start once, return to neutral once, nothing per
 * frame, forced close on release-all). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tearscape_gptk_receipt.h"
#include "nxinput_gptk_godot.h"

static void expect(bool p_condition, const char *p_message) {
	if (!p_condition) {
		fprintf(stderr, "TEARSCAPE GPTK RECEIPT: FAIL: %s\n", p_message);
		exit(1);
	}
}

static void expect_line(const char *p_got, const char *p_want, const char *p_message) {
	if (strcmp(p_got, p_want) != 0) {
		fprintf(stderr, "TEARSCAPE GPTK RECEIPT: FAIL: %s\n  got:  %s\n  want: %s\n",
				p_message, p_got, p_want);
		exit(1);
	}
}

static const char *const SCHEMA = "nxinput-gptk-event-evidence/1";

static size_t count_lines(const char *p_path, char *p_last, size_t p_last_size) {
	FILE *in = fopen(p_path, "r");
	expect(in != NULL, "the receipt file exists");
	char buffer[1024];
	size_t lines = 0;
	if (p_last_size > 0) {
		p_last[0] = '\0';
	}
	while (fgets(buffer, sizeof(buffer), in)) {
		lines++;
		size_t length = strlen(buffer);
		expect(length > 0 && buffer[length - 1] == '\n', "every receipt line ends with a newline");
		buffer[length - 1] = '\0';
		if (p_last_size > 0) {
			snprintf(p_last, p_last_size, "%s", buffer);
		}
	}
	fclose(in);
	return lines;
}

int main() {
	char line[640];

	/* --- the adapter sink-id table --------------------------------------- */
	size_t count = 0;
	const TearsGptkAdapterSink *rows = tears_gptk_adapter_sinks(&count);
	expect(rows != NULL && count == 16, "sixteen declared actions carry a sink id");
	for (size_t i = 1; i < count; i++) {
		expect(strcmp(rows[i - 1].action, rows[i].action) < 0,
				"the table is sorted by action (one row per action)");
	}
	expect(strcmp(tears_gptk_adapter_sink_id("system.pause"), "engine.input.pause") == 0,
			"system.pause maps to the ADAPTER sink id, never the InputMap name");
	expect(strcmp(tears_gptk_adapter_sink_id("system.quit"), "adapter.system.quit") == 0,
			"system.quit maps to the adapter lifecycle sink");
	expect(strcmp(tears_gptk_adapter_sink_id("player.move"), "engine.input.move") == 0 &&
					strcmp(tears_gptk_adapter_sink_id("menu.navigate"), "engine.ui_direction") == 0,
			"the vector actions map to their adapter sink ids");
	expect(strcmp(tears_gptk_adapter_sink_id("player.select_previous"), "engine.input.select_prev") == 0,
			"select_previous keeps the adapter's select_prev id");
	expect(tears_gptk_adapter_sink_id("system.start_coop") == NULL &&
					tears_gptk_adapter_sink_id(NULL) == NULL,
			"an undeclared action has no sink id");

	/* --- the four line shapes, byte-exact --------------------------------- */
	expect(tears_gptk_receipt_runtime_line(line, sizeof(line), SCHEMA,
					"nxinput-gptk-runtime/3",
					"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
					"default", 3, "auto", 18) == 0,
			"runtime line formats");
	expect_line(line,
			"{\"schema\":\"nxinput-gptk-event-evidence/1\",\"kind\":\"runtime\","
			"\"marker\":\"nxinput-gptk-runtime/3\",\"mapping_sha256\":"
			"\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
			"\"source\":\"default\",\"gptk_schema\":3,\"face_layout\":\"auto\",\"sinks\":18}",
			"runtime line shape");

	expect(tears_gptk_receipt_context_line(line, sizeof(line), SCHEMA, "menu", "scene:ui") == 0,
			"context line formats");
	expect_line(line,
			"{\"schema\":\"nxinput-gptk-event-evidence/1\",\"kind\":\"context\","
			"\"context\":\"menu\",\"source\":\"scene:ui\",\"observed\":true}",
			"context line shape");

	expect(tears_gptk_receipt_delivery_line(line, sizeof(line), SCHEMA, "menu",
					"scene:ui", "START", "press", "system.pause", "engine.input.pause", true) == 0,
			"delivery press line formats");
	expect_line(line,
			"{\"schema\":\"nxinput-gptk-event-evidence/1\",\"kind\":\"delivery\","
			"\"context\":\"menu\",\"context_source\":\"scene:ui\",\"control\":\"START\","
			"\"event\":\"press\",\"decision\":\"ACTION\",\"action\":\"system.pause\","
			"\"sink\":\"engine.input.pause\",\"pressed\":1,\"delivery_count\":1}",
			"delivery press line shape");
	expect(tears_gptk_receipt_delivery_line(line, sizeof(line), SCHEMA, "gameplay",
					"scene:world", "LEFT_STICK", "motion", "player.move", "engine.input.move", false) == 0,
			"delivery motion line formats");
	expect_line(line,
			"{\"schema\":\"nxinput-gptk-event-evidence/1\",\"kind\":\"delivery\","
			"\"context\":\"gameplay\",\"context_source\":\"scene:world\","
			"\"control\":\"LEFT_STICK\",\"event\":\"motion\",\"decision\":\"ACTION\","
			"\"action\":\"player.move\",\"sink\":\"engine.input.move\",\"pressed\":0,"
			"\"delivery_count\":1}",
			"delivery motion (return to neutral) line shape");

	expect(tears_gptk_receipt_suppressed_line(line, sizeof(line), SCHEMA, "gameplay",
					"scene:world", "R3") == 0,
			"suppressed line formats");
	expect_line(line,
			"{\"schema\":\"nxinput-gptk-event-evidence/1\",\"kind\":\"suppressed\","
			"\"context\":\"gameplay\",\"context_source\":\"scene:world\",\"control\":\"R3\","
			"\"event\":\"press\",\"decision\":\"SUPPRESS\",\"delivery_count\":0}",
			"suppressed line shape");

	/* NULL strings print as "" and never crash; a short buffer refuses. */
	expect(tears_gptk_receipt_context_line(line, sizeof(line), SCHEMA, "menu", NULL) == 0 &&
					strstr(line, "\"source\":\"\"") != NULL,
			"a NULL source prints as an empty string");
	char tiny[16];
	expect(tears_gptk_receipt_context_line(tiny, sizeof(tiny), SCHEMA, "menu", "x") == -1 &&
					tiny[0] == '\0',
			"a line that does not fit is refused, never truncated JSON");
	expect(tears_gptk_receipt_context_line(NULL, 0, SCHEMA, "menu", "x") == -1,
			"a NULL buffer is refused");

	/* --- the receipt file ------------------------------------------------- */
	TearsGptkReceipt off;
	tears_gptk_receipt_init(&off);
	expect(!tears_gptk_receipt_open(&off, NULL) && !tears_gptk_receipt_open(&off, ""),
			"no path = receipt silently disabled");
	tears_gptk_receipt_write(&off, "{\"never\":true}"); /* must be a no-op */
	expect(off.out == NULL && off.tried, "a disabled receipt stays closed and is tried once");

	const char *tmpdir = getenv("TMPDIR");
	char path[512];
	snprintf(path, sizeof(path), "%s/tearscape-gptk-receipt-test-%ld.jsonl",
			tmpdir && tmpdir[0] ? tmpdir : ".", (long)getpid());
	unlink(path);
	FILE *seed = fopen(path, "w");
	expect(seed != NULL, "the test receipt path is writable");
	fputs("{\"seed\":1}\n", seed);
	fclose(seed);

	TearsGptkReceipt receipt;
	tears_gptk_receipt_init(&receipt);
	expect(tears_gptk_receipt_open(&receipt, path), "the receipt opens");
	expect(tears_gptk_receipt_open(&receipt, "/nonexistent/other") && receipt.out != NULL,
			"a second open is a no-op on the already open file");
	tears_gptk_receipt_write(&receipt, "{\"a\":1}");
	char last[1024];
	/* Line-buffered: the line is on disk before any flush/close. */
	expect(count_lines(path, last, sizeof(last)) == 2 && strcmp(last, "{\"a\":1}") == 0,
			"append mode keeps the truncating launcher's content and the line is flushed per line");
	tears_gptk_receipt_write(&receipt, NULL); /* no-op */
	tears_gptk_receipt_write(&receipt, "{\"b\":2}");
	expect(count_lines(path, last, sizeof(last)) == 3 && strcmp(last, "{\"b\":2}") == 0,
			"each write is exactly one line");
	fclose(receipt.out);
	unlink(path);

	/* --- release attribution ------------------------------------------------ */
	TearsGptkPressOwner owner;
	tears_gptk_press_owner_init(&owner);
	expect(owner.control == -1, "nothing pressed at init");
	expect(tears_gptk_press_owner_edge(&owner, true, 9) == 9 && owner.control == 9,
			"a press is attributed to the feeding control and remembered");
	/* The release arrives from a context change: no control is feeding. */
	expect(tears_gptk_press_owner_edge(&owner, false, -1) == 9,
			"a release outside any feed is attributed to the control that pressed");
	expect(owner.control == -1, "the release forgets the owner");
	expect(tears_gptk_press_owner_edge(&owner, true, 2) == 2 &&
					tears_gptk_press_owner_edge(&owner, false, 5) == 2,
			"a release fed by another control still names the pressing control");
	expect(tears_gptk_press_owner_edge(&owner, false, -1) == -1,
			"a release with no owner names no control");

	/* --- vector EDGE semantics ------------------------------------------- */
	TearsGptkVectorGesture gesture;
	tears_gptk_vector_gesture_init(&gesture);
	int starts = 0, stops = 0, nones = 0;
	/* 30 neutral frames: nothing. */
	for (int frame = 0; frame < 30; frame++) {
		expect(tears_gptk_vector_gesture_feed(&gesture, false) == TEARS_GPTK_VECTOR_EDGE_NONE,
				"idle never alternates press/release");
	}
	/* 60 deflected frames: START exactly once. */
	for (int frame = 0; frame < 60; frame++) {
		const int edge = tears_gptk_vector_gesture_feed(&gesture, true);
		if (edge == TEARS_GPTK_VECTOR_EDGE_START) {
			starts++;
		} else if (edge == TEARS_GPTK_VECTOR_EDGE_STOP) {
			stops++;
		} else {
			nones++;
		}
	}
	expect(starts == 1 && stops == 0 && nones == 59 && gesture.active,
			"leaving neutral opens the gesture once; held frames are silent");
	/* 30 neutral frames: STOP exactly once. */
	for (int frame = 0; frame < 30; frame++) {
		const int edge = tears_gptk_vector_gesture_feed(&gesture, false);
		if (edge == TEARS_GPTK_VECTOR_EDGE_STOP) {
			stops++;
		} else {
			expect(edge == TEARS_GPTK_VECTOR_EDGE_NONE, "neutral frames after the stop are silent");
		}
	}
	expect(stops == 1 && !gesture.active, "return to neutral closes the gesture once");
	/* A second gesture, then a release-all before neutral: forced close once. */
	expect(tears_gptk_vector_gesture_feed(&gesture, true) == TEARS_GPTK_VECTOR_EDGE_START,
			"a new gesture opens again");
	expect(tears_gptk_vector_gesture_close(&gesture) && !gesture.active,
			"release-all closes an open gesture (one pressed=0 line is due)");
	expect(!tears_gptk_vector_gesture_close(&gesture),
			"release-all on a closed gesture owes nothing");
	expect(tears_gptk_vector_gesture_feed(&gesture, false) == TEARS_GPTK_VECTOR_EDGE_NONE,
			"neutral after a forced close is silent");
	expect(tears_gptk_vector_gesture_feed(NULL, true) == TEARS_GPTK_VECTOR_EDGE_NONE &&
					!tears_gptk_vector_gesture_close(NULL),
			"NULL gestures are inert");

	/* Stick normalizer: the admission snapshot and the SDL event path share
	 * it, and a centered stick MUST be exactly 0 (the asymmetric
	 * (v+32768)/65535 form gave +1.5e-5, opening the gesture on admission). */
	expect(nxgptk_godot_stick_axis_value(0) == 0.0f, "centered stick normalizes to an exact 0");
	expect(nxgptk_godot_stick_axis_value(32767) == 1.0f && nxgptk_godot_stick_axis_value(-32768) == -1.0f,
			"full deflection reaches exactly +1/-1");
	expect(nxgptk_godot_stick_axis_value(16384) > 0.49f && nxgptk_godot_stick_axis_value(16384) < 0.51f &&
					nxgptk_godot_stick_axis_value(-16384) < -0.49f && nxgptk_godot_stick_axis_value(-16384) > -0.51f,
			"half deflection is symmetric");
	{
		struct TearsGptkVectorGesture centered;
		tears_gptk_vector_gesture_init(&centered);
		const float v = nxgptk_godot_stick_axis_value(0);
		expect(tears_gptk_vector_gesture_feed(&centered, v > 0.0f || v < 0.0f) == TEARS_GPTK_VECTOR_EDGE_NONE,
				"a stick at rest never opens the vector gesture");
	}

	printf("TEARSCAPE GPTK RECEIPT: ALL PASS\n");
	return 0;
}
