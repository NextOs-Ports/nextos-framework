/* display_contract.c -- no-SDL/no-GL geometry used by runtime and host gates. */

#include <string.h>

#include "display_contract.h"

#define SF_NATIVE_WIDTH 1280
#define SF_NATIVE_HEIGHT 720
#define SF_MAX_DRAWABLE 16384

const char *sf_display_policy_name(enum sf_aspect_policy policy)
{
    return policy == SF_ASPECT_NATIVE ? "native" : "fill";
}

int sf_display_fill_source_rect(int drawable_width, int drawable_height,
                                sf_display_rect *source)
{
    if (!source || drawable_width <= 0 || drawable_height <= 0 ||
        drawable_width > SF_MAX_DRAWABLE ||
        drawable_height > SF_MAX_DRAWABLE)
        return -1;

    source->x = 0;
    source->y = 0;
    source->width = drawable_width;
    source->height = drawable_height;

    long long physical = (long long)drawable_width * SF_NATIVE_HEIGHT;
    long long native = (long long)drawable_height * SF_NATIVE_WIDTH;
    if (physical == native)
        return 0;

    if (physical < native) {
        /* Narrow display (for example 640x480): crop the centered 16:9
         * picture, then the presenter expands it vertically. */
        int height = (drawable_width * SF_NATIVE_HEIGHT +
                      SF_NATIVE_WIDTH / 2) / SF_NATIVE_WIDTH;
        if (height <= 0 || height > drawable_height)
            return -1;
        source->height = height;
        source->y = (drawable_height - height) / 2;
    } else {
        /* Wider than 16:9: crop the centered 16:9 picture horizontally. */
        int width = (drawable_height * SF_NATIVE_WIDTH +
                     SF_NATIVE_HEIGHT / 2) / SF_NATIVE_HEIGHT;
        if (width <= 0 || width > drawable_width)
            return -1;
        source->width = width;
        source->x = (drawable_width - width) / 2;
    }
    return 1;
}

sf_display_contract sf_display_contract_make(int drawable_width,
                                             int drawable_height,
                                             const char *requested_policy)
{
    sf_display_contract result = {
        .policy = SF_ASPECT_FILL,
        .drawable_width = drawable_width,
        .drawable_height = drawable_height,
        .android_width = drawable_width,
        .android_height = drawable_height,
        .lock_android_size = 1,
        .identity = 1,
        .requested_policy_valid = 1,
    };

    if (drawable_width <= 0 || drawable_width > SF_MAX_DRAWABLE ||
        drawable_height <= 0 || drawable_height > SF_MAX_DRAWABLE) {
        result.drawable_width = SF_NATIVE_WIDTH;
        result.drawable_height = SF_NATIVE_HEIGHT;
        result.android_width = SF_NATIVE_WIDTH;
        result.android_height = SF_NATIVE_HEIGHT;
    }

    /* Preserve the proven mutable Android buffer path on an already-native
     * 16:9 drawable.  The lock exists only to prevent a non-16:9 fill contract
     * from drifting back to the historical 16:9 size. */
    if (result.drawable_width * SF_NATIVE_HEIGHT ==
        result.drawable_height * SF_NATIVE_WIDTH)
        result.lock_android_size = 0;

    if (!requested_policy || !*requested_policy ||
        strcmp(requested_policy, "fill") == 0) {
        return result;
    }

    if (strcmp(requested_policy, "native") == 0) {
        result.policy = SF_ASPECT_NATIVE;
        result.android_width = SF_NATIVE_WIDTH;
        result.android_height = SF_NATIVE_HEIGHT;
        /* Rollback means the old mutable Android buffer geometry remains
         * available.  The SDL/EGL drawable itself is never resized here. */
        result.lock_android_size = 0;
        result.identity = result.drawable_width == result.android_width &&
                          result.drawable_height == result.android_height;
        return result;
    }

    result.requested_policy_valid = 0;
    return result;
}
