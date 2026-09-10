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
 *   nxgl_frame_proof_before_present(w, h);      // immediately before swap
 *   nxgl_frame_proof_publish();                 // after the last sample, and
 *                                               // again at shutdown
 *
 * `publish` is idempotent, and is meant to be called at the last sample as well
 * as at shutdown: an automated run ends with SIGKILL, not a clean exit, so a
 * verdict emitted only on shutdown is missing from exactly the runs that need
 * it.
 *
 * The verdict is asymmetric. A measured visible frame proves drawing only at
 * an explicit before-present boundary; an empty frame accuses the port only
 * after three sequential before-present samples and when the launch could have
 * produced an image. On some firmware a remote shell cannot open a window for
 * reasons that say nothing about the game.
 *
 * The port owns the GL context. The adapter refuses a non-default draw/read
 * framebuffer, a pixel-pack buffer, nonzero pack row/skip state, an unproved
 * state query, unsafe dimensions and a read that did not overwrite two
 * independent initialized sentinels. It does not change bindings or pack state
 * and never calls glGetError, so the game's error queue is not consumed.
 *
 * Since nxgl 0.3.2, when NXBOOTSTRAP_VIDEO_FILE and the three
 * NXBOOTSTRAP_HEALTH_{RUN_ID,GENERATION,PORT_ID} tuple fields are present, the
 * adapter also publishes the private org.nextos.nxruntime.video-proof JSON
 * consumed by the launcher watchdog. OK counts only RGB-nonblack pixels whose
 * alpha is nonzero: RGB with alpha zero is BLACK (the Amlogic OSD compositor
 * case). Conclusive BLACK/DEAD-CONTEXT needs three sequential observations and
 * is irreversible. If fatal replacement fails after OK, the old receipt is
 * revoked and later calls retry; an unrelated colliding temp is never removed.
 * This adapter never creates or validates generation health. Consuming a
 * conclusive fatal safely removes the exact health path captured at launch,
 * when such a regular owner-only receipt exists, so an engine cannot leave a
 * stale ready receipt behind while shutting down nonzero.
 *
 * All stateful entry points are render-thread APIs protected by a non-blocking
 * reentrancy/concurrency guard. The owner keeps the guard during measurement,
 * but entry performs one test-and-set and never waits or spins: a GL callback
 * that reenters is refused immediately, so it cannot deadlock on itself.
 * "alpha nonzero" is a literal buffer contract (1..255); it closes the proven
 * alpha==0 compositor failure and makes no broader perceptual-brightness claim.
 */

/* Optional: install the port's own GL symbol resolver. A so-loader port routes
 * every gl* call through its shim, and the real glReadPixels exists nowhere
 * else. Without this the adapter falls back to dlsym(RTLD_DEFAULT). */
void nxgl_frame_proof_set_resolver(void *(*resolver)(const char *));

/* Emit the launch receipt. Call once, early, before anything can fail. */
void nxgl_frame_proof_launch_receipt(void);

/* Optional (E2): register the video context once it exists, so the final
 * "VIDEO:" receipt names the window and the GL stack a reader needs. Any
 * argument may be 0/NULL; the receipt then prints "?" for that field. Strings
 * are copied (bounded), so SDL_GetError()/glGetString results may be passed
 * directly. */
void nxgl_frame_proof_set_video_context(int width, int height,
                                        const char *driver,
                                        const char *renderer,
                                        const char *version);

/* Legacy diagnostic sample with an unspecified presentation point. It may
 * contribute to the human VIDEO line but cannot publish machine OK/BLACK. New
 * integrations must use before_present() at the real swap boundary. */
void nxgl_frame_proof_sample(int width, int height);

/* V3: WHERE a sample was taken, recorded into the receipt. On tile-based GPUs
 * the backbuffer content is UNDEFINED after the swap, so an after-present
 * readback can be a false BLACK -- a receipt that does not say where it
 * sampled cannot be audited. nxgl_frame_proof_sample() keeps its historical
 * meaning (point unspecified); nxgl_frame_proof_before_present() records
 * before-present; a port that samples explicitly may use sample_at(BEFORE)
 * only at that same real boundary. AFTER and unspecified never authorize a
 * machine verdict. The "VIDEO:" receipt line keeps its field layout and the
 * APPENDED
 * "sample_point=" field (before-present | after-present | unspecified |
 * mixed | none); an unspecified visible sample now honestly reports
 * INCONCLUSIVE instead of the historical false OK. */
typedef enum nxgl_frame_proof_sample_point {
  NXGL_PROOF_BEFORE_PRESENT = 0,
  NXGL_PROOF_AFTER_PRESENT = 1
} nxgl_frame_proof_sample_point;

void nxgl_frame_proof_sample_at(int width, int height,
                                nxgl_frame_proof_sample_point point);

/* Publish the current log verdict and retry a pending fatal receipt. One black
 * or dead sample remains INCONCLUSIVE; this call cannot turn it fatal. */
void nxgl_frame_proof_publish(void);

/* Onda v2 (23/08/2026): prova de imagem CONTINUA, um chamado por quadro,
 * imediatamente ANTES do present -- depois do swap o conteudo do backbuffer
 * e' indefinido em GPU tile-based e a leitura vira falso PRETO.
 *
 * O adapter amostra sozinho num cronograma esparso (quadros 30, 120, 600 e
 * depois a cada 1800), publica o veredito na ultima amostra do cronograma
 * inicial e, quando 3 amostras SEGUIDAS ficam pretas com os quadros andando,
 * grita uma linha "IMAGE PROOF" + NXEVENT 6304 -- o caso Brotato (jogo vivo,
 * renderer saudavel, painel preto) deixa de ser invisivel no log. Custo fora
 * das amostras: um incremento por quadro. NXGL_IMAGE_PROOF=0 desliga a
 * amostragem automatica (as chamadas manuais de sample/publish continuam
 * valendo). */
void nxgl_frame_proof_before_present(int width, int height);

/* A conclusive BLACK/DEAD-CONTEXT is process-global and irreversible. The
 * persistent query remains true forever; consume returns 1 exactly once and
 * revokes any safe launch-captured health receipt before handing ownership of
 * the close request to the engine adapter. */
int nxgl_frame_proof_is_fatal(void);
int nxgl_frame_proof_consume_fatal(void);

#endif
