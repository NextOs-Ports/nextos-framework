#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/capture_x86.c"

int main(void)
{
    static const unsigned char rgba[] = {
        /* OpenGL bottom row. */
        255, 0, 0, 255, 0, 255, 0, 255,
        /* OpenGL top row. */
        0, 0, 255, 255, 255, 255, 255, 255,
    };
    static const unsigned char expected[] = {
        'P', '6', '\n', '2', ' ', '2', '\n', '2', '5', '5', '\n',
        0, 0, 255, 255, 255, 255,
        255, 0, 0, 0, 255, 0,
    };
    static const char expected_sha256[] =
        "adda0c5d2a09444c9c3f45a79a2ebba15c7d3abcb14c9564fcf698ca78952935";
    char output_path[] = "/tmp/asm2-capture-ppm-test.XXXXXX";
    char digest_hex[65];
    unsigned char actual[sizeof(expected)];
    size_t written_size = 0;

    int descriptor = mkstemp(output_path);
    if (descriptor < 0) {
        perror("mkstemp");
        return 1;
    }
    close(descriptor);
    unlink(output_path);
    memcpy(asm2_capture.output, output_path, strlen(output_path) + 1u);

    if (asm2_capture_write_ppm(rgba, 2, 2, digest_hex, &written_size) != 0) {
        perror("asm2_capture_write_ppm");
        return 2;
    }
    FILE *input = fopen(output_path, "rb");
    if (!input) {
        perror("fopen");
        return 3;
    }
    const size_t actual_size = fread(actual, 1, sizeof(actual), input);
    const int extra = fgetc(input);
    fclose(input);
    unlink(output_path);

    if (written_size != sizeof(expected) ||
        actual_size != sizeof(expected) || extra != EOF ||
        memcmp(actual, expected, sizeof(expected)) != 0) {
        fprintf(stderr,
                "ASM2_CAPTURE_PPM_TEST_FAIL bytes=%zu/%zu/%zu extra=%d\n",
                written_size, actual_size, sizeof(expected), extra);
        return 4;
    }
    if (strcmp(digest_hex, expected_sha256) != 0) {
        fprintf(stderr,
                "ASM2_CAPTURE_PPM_TEST_FAIL sha256=%s expected=%s\n",
                digest_hex, expected_sha256);
        return 5;
    }
    fprintf(stderr,
            "ASM2_CAPTURE_PPM_TEST_OK bytes=%zu sha256=%s flipped=1\n",
            written_size, digest_hex);
    return 0;
}
