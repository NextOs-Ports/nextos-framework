#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "viewport_policy.h"

static int assertions;

static void require(int condition, const char *message)
{
    assertions++;
    if (!condition) {
        fprintf(stderr, "adaptive viewport FAIL: %s\n", message);
        exit(1);
    }
}

static void prove_containment(int width, int height, float expected_match)
{
    const float reference_width = 800.0f;
    const float reference_height = 420.0f;
    float match = -1.0f;
    require(nc_viewport_containment_match(width, height,
                                          reference_width, reference_height,
                                          &match) == 0,
            "valid dimensions rejected");
    require(match == expected_match, "wrong CanvasScaler endpoint");

    float scale = match == 0.0f
                    ? (float)width / reference_width
                    : (float)height / reference_height;
    float logical_width = (float)width / scale;
    float logical_height = (float)height / scale;
    require(logical_width + 0.01f >= reference_width,
            "reference width is clipped");
    require(logical_height + 0.01f >= reference_height,
            "reference height is clipped");
}

int main(void)
{
    require(!nc_viewport_should_probe(0, 0), "probed before first frame");
    require(!nc_viewport_should_probe(15, 0), "probed at frame 15");
    require(!nc_viewport_should_probe(29, 0), "probed at frame 29");
    require(nc_viewport_should_probe(30, 0), "did not probe at frame 30");
    require(nc_viewport_should_probe(45, 0), "did not probe at frame 45");
    require(!nc_viewport_should_probe(45, 1), "probed during gameplay");

    prove_containment(640, 480, 0.0f);
    prove_containment(1280, 720, 0.0f);
    prove_containment(1920, 1080, 0.0f);
    prove_containment(2560, 1080, 1.0f);

    float match = 0.0f;
    require(nc_viewport_containment_match(0, 480, 800.0f, 420.0f,
                                          &match) != 0,
            "zero drawable width accepted");
    require(nc_viewport_containment_match(640, 480, NAN, 420.0f,
                                          &match) != 0,
            "NaN reference accepted");
    require(nc_viewport_containment_match(640, 480, 800.0f, 420.0f,
                                          NULL) != 0,
            "null output accepted");

    printf("ADAPTIVE VIEWPORT PASS: %d assertions\n", assertions);
    return 0;
}
