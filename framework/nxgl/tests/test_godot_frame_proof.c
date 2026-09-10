/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxgl_godot_frame_proof.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char order[16];
static size_t order_len;
static void *(*installed_resolver)(const char *);
static int symbol_token;
static int fatal_mode;
static int fatal_active;
static int fatal_pending;
static int sample_calls;
static int health_revocations;

static void record(char value) {
	if (order_len < sizeof order)
		order[order_len++] = value;
}

void nxgl_frame_proof_launch_receipt(void) { record('L'); }
void nxgl_frame_proof_set_resolver(void *(*resolver)(const char *)) {
	installed_resolver = resolver;
	record('R');
}
void nxgl_frame_proof_set_video_context(int width, int height,
		const char *driver, const char *renderer, const char *version) {
	(void)renderer;
	(void)version;
	if (width != 640 || height != 480 || strcmp(driver, "sdl2") != 0 ||
			installed_resolver == 0 ||
			installed_resolver("glReadPixels") != &symbol_token)
		exit(2);
	record('C');
}
void nxgl_frame_proof_before_present(int width, int height) {
	if (width != 640 || height != 480)
		exit(3);
	sample_calls++;
	record('P');
	if (fatal_mode != 0 && sample_calls == 3) {
		fatal_active = 1;
		fatal_pending = 1;
	}
}
void nxgl_frame_proof_publish(void) { record('S'); }
int nxgl_frame_proof_is_fatal(void) { return fatal_active; }
int nxgl_frame_proof_consume_fatal(void) {
	int pending = fatal_pending;
	if (pending) {
		fatal_pending = 0;
		health_revocations++;
	}
	return pending;
}

void nxgl_frame_proof_sample(int width, int height) {
	(void)width; (void)height;
}
void nxgl_frame_proof_sample_at(int width, int height,
		nxgl_frame_proof_sample_point point) {
	(void)width; (void)height; (void)point;
}

static void *resolve(const char *name) {
	return strcmp(name, "glReadPixels") == 0 ? &symbol_token : 0;
}

static void expect(int condition, const char *message) {
	if (!condition) {
		fprintf(stderr, "nxgl Godot frame proof: FAIL: %s\n", message);
		exit(1);
	}
}

static void reset_fatal_stub(int mode) {
	fatal_mode = mode;
	fatal_active = 0;
	fatal_pending = 0;
	sample_calls = 0;
	health_revocations = 0;
}

static void exercise_fatal(int mode, const char *label) {
	nxgl_godot_frame_proof proof = NXGL_GODOT_FRAME_PROOF_INIT;
	int presents = 0;
	reset_fatal_stub(mode);
	expect(nxgl_godot_frame_proof_begin(&proof) == 0, label);
	expect(nxgl_godot_frame_proof_context(
			&proof, resolve, 640, 480, "sdl2", "mali", "gles2") == 0,
			label);
	for (int sample = 0; sample < 2; sample++) {
		expect(nxgl_godot_frame_proof_before_swap(&proof, 640, 480) == 0,
				label);
		presents++;
	}
	expect(nxgl_godot_frame_proof_before_swap(&proof, 640, 480) ==
			NXGL_GODOT_FRAME_PROOF_FATAL, label);
	expect(sample_calls == 3 && presents == 2, label);
	expect(nxgl_godot_frame_proof_before_swap(&proof, 640, 480) ==
			NXGL_GODOT_FRAME_PROOF_FATAL && sample_calls == 3,
			"fatal blocks every later present before a fourth sample");
	expect(health_revocations == 1,
			"fatal consumption revokes health exactly once");
	expect(nxgl_godot_frame_proof_consume_close(&proof) == 1 &&
			nxgl_godot_frame_proof_consume_close(&proof) == 0,
			"fatal close request is one-shot");
	expect(nxgl_godot_frame_proof_health_allowed(&proof) == 0 &&
			nxgl_godot_frame_proof_exit_status(&proof) ==
				NXGL_GODOT_FRAME_PROOF_FATAL_STATUS,
			"fatal permanently blocks health and preserves nonzero status");
	expect(nxgl_godot_frame_proof_stop(&proof) ==
			NXGL_GODOT_FRAME_PROOF_FATAL,
			"fatal stop cannot become clean");
	expect(nxgl_godot_frame_proof_consume_close(&proof) == 0,
			"fatal stop cannot re-arm an already consumed close request");
}

int main(void) {
	nxgl_godot_frame_proof proof = NXGL_GODOT_FRAME_PROOF_INIT;
	expect(nxgl_godot_frame_proof_before_swap(&proof, 640, 480) != 0,
			"present before context is refused");
	expect(nxgl_godot_frame_proof_begin(&proof) == 0,
			"early launch receipt succeeds once");
	expect(nxgl_godot_frame_proof_begin(&proof) != 0,
			"duplicate launch is refused");
	expect(nxgl_godot_frame_proof_context(
			&proof, resolve, 640, 480, "sdl2", "mali", "gles2") == 0,
			"current context installs resolver before metadata");
	expect(nxgl_godot_frame_proof_before_swap(&proof, 640, 480) == 0,
			"real pre-swap boundary samples");
	expect(nxgl_godot_frame_proof_stop(&proof) == 0,
			"shutdown publishes before context destruction");
	expect(nxgl_godot_frame_proof_health_allowed(&proof) == 1 &&
			nxgl_godot_frame_proof_exit_status(&proof) == 0 &&
			nxgl_godot_frame_proof_consume_close(&proof) == 0,
			"OK never closes, blocks health or changes exit status");
	expect(nxgl_godot_frame_proof_before_swap(&proof, 640, 480) != 0 &&
			nxgl_godot_frame_proof_stop(&proof) != 0,
			"post-shutdown reuse is refused");
	expect(order_len == 5u && memcmp(order, "LRCPS", 5u) == 0,
			"hook order is launch-resolver-context-present-stop");
	expect(strcmp(nxgl_godot_frame_proof_marker(),
			"nxgl-godot-frame-proof/2") == 0,
			"source glue marker is stable");
	exercise_fatal(1, "three BLACK samples become fatal");
	exercise_fatal(2, "DEAD-CONTEXT becomes fatal");
	puts("nxgl Godot frame proof: PASS order=LRCPS no-synthetic-present=1 "
			"black-fatal=3 dead-fatal=1 no-fourth-present=1 ok-open=1");
	return 0;
}
