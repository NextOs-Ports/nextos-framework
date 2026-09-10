#include "viewport_policy.h"

#include <math.h>

int nc_viewport_should_probe(unsigned long frame, int gameplay_active)
{
    return !gameplay_active && frame >= 30 && frame % 15 == 0;
}

int nc_viewport_containment_match(int screen_width, int screen_height,
                                  float reference_width,
                                  float reference_height,
                                  float *match_out)
{
    if (!match_out || screen_width <= 0 || screen_height <= 0 ||
        !isfinite(reference_width) || !isfinite(reference_height) ||
        reference_width <= 0.0f || reference_height <= 0.0f)
        return -1;

    float width_scale = (float)screen_width / reference_width;
    float height_scale = (float)screen_height / reference_height;
    *match_out = width_scale <= height_scale ? 0.0f : 1.0f;
    return 0;
}
