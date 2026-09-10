/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef OTR_RUNTIME_EVIDENCE_H
#define OTR_RUNTIME_EVIDENCE_H

/* Executes the declared GLES2/ESSL100 contract against the live SDL context
 * and persists the framework JSON.  A non-zero result is the only permission
 * to enter the engine's shader/nativeInit path. */
int otr_graphics_contract_start(const char *game_dir);

/* Sparse, read-only pre-present probe of the NATIVE game frame (called before
 * the cursor overlay).  It records FBO/viewport/scissor/mask state and emits
 * the exact nxbootstrap health receipt only after a genuinely non-black frame.
 * Returns 1 once health has been published, otherwise 0. */
int otr_graphics_pre_present(unsigned long frame, int width, int height);

/* Persist the current GPTK counters alongside the graphics evidence. */
const char *otr_input_evidence_path(void);
const char *otr_input_load_evidence_path(void);
const char *otr_audio_evidence_path(void);

#endif /* OTR_RUNTIME_EVIDENCE_H */
