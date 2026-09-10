#ifndef NAMELESSCAT_VIEWPORT_POLICY_H
#define NAMELESSCAT_VIEWPORT_POLICY_H

/* Select Unity CanvasScaler's Match Width Or Height endpoint that contains
 * the complete reference canvas in the physical drawable. */
int nc_viewport_containment_match(int screen_width, int screen_height,
                                  float reference_width,
                                  float reference_height,
                                  float *match_out);

int nc_viewport_should_probe(unsigned long frame, int gameplay_active);

#endif
