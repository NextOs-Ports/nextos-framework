/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXGL_FRAME_PROOF_ADAPTER_H
#define NXGL_FRAME_PROOF_ADAPTER_H

/* Drop-in frame proof for a port that owns an SDL/GLES context.
 *
 * A port that renders nothing is indistinguishable from a healthy one in every
 * signal a launcher has: the loop ticks, audio plays, input arrives and the
 * process exits 0. The only thing that ever caught a black screen was a person
 * looking at the panel, and when screenshots stood in for that person the empty
 * files were filed as proof of gameplay.
 *
 * Wiring, three calls:
 *
 *   nxgl_frame_proof_launch_receipt();          // once, before GL can fail
 *   nxgl_frame_proof_sample(w, h);              // at a few frames in the loop
 *   nxgl_frame_proof_publish();                 // after the last sample, and
 *                                               // again at shutdown
 *
 * `publish` is idempotent, and is meant to be called at the last sample as well
 * as at shutdown: an automated run ends with SIGKILL, not a clean exit, so a
 * verdict emitted only on shutdown is missing from exactly the runs that need
 * it.
 *
 * The verdict is asymmetric. A drawn frame proves the port draws however it was
 * launched; an empty frame only accuses the port when the launch could have
 * produced an image, which is why the launch receipt exists. On some firmware a
 * port started from a remote shell cannot open a window at all for reasons that
 * say nothing about the game.
 *
 * The port owns the GL context; this reads it back with glReadPixels on the
 * default framebuffer and never changes GL state that it does not restore.
 */

/* Optional: install the port's own GL symbol resolver. A so-loader port routes
 * every gl* call through its shim, and the real glReadPixels exists nowhere
 * else. Without this the adapter falls back to dlsym(RTLD_DEFAULT). */
void nxgl_frame_proof_set_resolver(void *(*resolver)(const char *));

/* Emit the launch receipt. Call once, early, before anything can fail. */
void nxgl_frame_proof_launch_receipt(void);

/* Read back the presented frame and record it. Call at more than one frame: a
 * title card can legitimately be black at the first sample, and one reading
 * turns that into a false verdict. */
void nxgl_frame_proof_sample(int width, int height);

/* Publish one verdict as a log line and as an NXEVENT. Idempotent. */
void nxgl_frame_proof_publish(void);

#endif
