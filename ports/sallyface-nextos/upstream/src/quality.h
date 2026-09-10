/* Official Sally Face quality profiles backed by the V3 settings contract. */
#ifndef SF_QUALITY_H
#define SF_QUALITY_H

#include "nxgl_quality.h"

typedef struct sf_quality_knobs {
    int tex_half_min;
    int etc1_enabled;
    int etc1_min;
} sf_quality_knobs;

nxgl_quality_level sf_quality_recommend(long mem_total_kb);
int sf_quality_profile_knobs(nxgl_quality_level level,
                             sf_quality_knobs *knobs);
void sf_quality_init(const char *gamedir);
void sf_quality_tick(unsigned long frame);

#endif /* SF_QUALITY_H */
