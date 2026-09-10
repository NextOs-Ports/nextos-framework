/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXGL_GODOT_FRAME_PROOF_H
#define NXGL_GODOT_FRAME_PROOF_H

/* Source-only lifecycle glue shared by Godot 3/4 ports. The engine-major
 * adapter owns context creation and supplies the resolver belonging to the
 * context it actually made current. This wrapper fixes the five hook order;
 * it never creates a window, context, frame or synthetic present. */

#ifdef __cplusplus
extern "C" {
#endif
#include "nxgl_frame_proof_adapter.h"
#ifdef __cplusplus
}
#endif

#define NXGL_GODOT_FRAME_PROOF_API_VERSION 2u
#define NXGL_GODOT_FRAME_PROOF_MARKER "nxgl-godot-frame-proof/2"
#define NXGL_GODOT_FRAME_PROOF_FATAL (-2)
#define NXGL_GODOT_FRAME_PROOF_FATAL_STATUS 72
#define NXGL_GODOT_FRAME_PROOF_INIT { 0u, 0u, 0u, 0u, 0u }

typedef struct nxgl_godot_frame_proof {
	unsigned int launched;
	unsigned int context_ready;
	unsigned int stopped;
	unsigned int fatal;
	unsigned int close_pending;
} nxgl_godot_frame_proof;

static inline void nxgl_godot_frame_proof_fail(
		nxgl_godot_frame_proof *proof) {
	if (proof != 0 && proof->fatal == 0u) {
		proof->fatal = 1u;
		proof->close_pending = 1u;
	}
}

static inline int nxgl_godot_frame_proof_begin(
		nxgl_godot_frame_proof *proof) {
	if (proof == 0 || proof->launched || proof->context_ready || proof->stopped ||
			proof->fatal)
		return -1;
	nxgl_frame_proof_launch_receipt();
	proof->launched = 1u;
	return 0;
}

static inline int nxgl_godot_frame_proof_context(
		nxgl_godot_frame_proof *proof,
		void *(*resolver)(const char *), int width, int height,
		const char *provider, const char *renderer, const char *version) {
	if (proof == 0 || !proof->launched || proof->context_ready ||
			proof->stopped || proof->fatal || resolver == 0 ||
			width <= 0 || height <= 0)
		return -1;
	/* Resolver first is mandatory: an RTLD_LOCAL SDL/EGL provider may make
	 * dlsym(RTLD_DEFAULT) unable to find even glReadPixels. */
	nxgl_frame_proof_set_resolver(resolver);
	nxgl_frame_proof_set_video_context(
			width, height, provider, renderer, version);
	proof->context_ready = 1u;
	return 0;
}
/* nxgl 0.3.5: the live window changed size after the context was registered.
 * Safe at any time between context() and stop(); refused otherwise. It only
 * updates the recorded geometry of the receipt -- the per-frame proof already
 * takes the live size through before_swap(). */
static inline int nxgl_godot_frame_proof_resize(
		nxgl_godot_frame_proof *proof, int width, int height) {
	if (proof == 0 || !proof->context_ready || proof->stopped ||
			width <= 0 || height <= 0)
		return -1;
	nxgl_frame_proof_set_video_size(width, height);
	return 0;
}


static inline int nxgl_godot_frame_proof_before_swap(
		nxgl_godot_frame_proof *proof, int width, int height) {
	if (proof == 0 || !proof->context_ready || proof->stopped ||
			width <= 0 || height <= 0)
		return -1;
	if (proof->fatal)
		return NXGL_GODOT_FRAME_PROOF_FATAL;
	nxgl_frame_proof_before_present(width, height);
	if (nxgl_frame_proof_consume_fatal() || nxgl_frame_proof_is_fatal()) {
		nxgl_godot_frame_proof_fail(proof);
		return NXGL_GODOT_FRAME_PROOF_FATAL;
	}
	return 0;
}

static inline int nxgl_godot_frame_proof_stop(
		nxgl_godot_frame_proof *proof) {
	if (proof == 0 || !proof->launched || proof->stopped)
		return -1;
	nxgl_frame_proof_publish();
	if (nxgl_frame_proof_consume_fatal() || nxgl_frame_proof_is_fatal())
		nxgl_godot_frame_proof_fail(proof);
	proof->stopped = 1u;
	return proof->fatal ? NXGL_GODOT_FRAME_PROOF_FATAL : 0;
}

static inline int nxgl_godot_frame_proof_consume_close(
		nxgl_godot_frame_proof *proof) {
	int pending;
	if (proof == 0)
		return 0;
	pending = proof->close_pending != 0u;
	proof->close_pending = 0u;
	return pending;
}

static inline int nxgl_godot_frame_proof_health_allowed(
		const nxgl_godot_frame_proof *proof) {
	return proof != 0 && proof->fatal == 0u;
}

static inline int nxgl_godot_frame_proof_exit_status(
		const nxgl_godot_frame_proof *proof) {
	return proof != 0 && proof->fatal != 0u
			? NXGL_GODOT_FRAME_PROOF_FATAL_STATUS : 0;
}

static inline const char *nxgl_godot_frame_proof_marker(void) {
	return NXGL_GODOT_FRAME_PROOF_MARKER;
}

#endif
