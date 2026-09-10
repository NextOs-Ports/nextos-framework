#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nod.h"

static int fail(const char *what) {
    const char *detail = nod_error_message();
    fprintf(stderr, "%s: %s\n", what, detail != NULL ? detail : strerror(errno));
    return 1;
}

int main(int argc, char **argv) {
    NodHandle *disc = NULL;
    NodHandle *partition = NULL;
    NodHandle *file = NULL;
    enum NodNodeKind kind;
    uint32_t length = 0;
    FILE *out = NULL;
    uint8_t buffer[64 * 1024];
    uint32_t total = 0;
    int result = 1;

    if (argc != 4) {
        fprintf(stderr, "usage: %s DISC_IMAGE DISC_PATH OUTPUT\n", argv[0]);
        return 2;
    }
    if (nod_disc_open(argv[1], NULL, &disc) != NOD_RESULT_OK) {
        return fail("nod_disc_open");
    }
    if (nod_disc_open_partition(disc, 0, NULL, &partition) != NOD_RESULT_OK) {
        fail("nod_disc_open_partition");
        goto done;
    }
    const uint32_t index = nod_partition_find_file(partition, argv[2], &kind, &length);
    if (index == UINT32_MAX || kind != NOD_NODE_KIND_FILE) {
        fprintf(stderr, "disc file not found: %s\n", argv[2]);
        goto done;
    }
    if (nod_partition_open_file(partition, index, &file) != NOD_RESULT_OK) {
        fail("nod_partition_open_file");
        goto done;
    }
    out = fopen(argv[3], "wb");
    if (out == NULL) {
        perror("fopen");
        goto done;
    }
    while (total < length) {
        const size_t want = length - total < sizeof(buffer) ? length - total : sizeof(buffer);
        const int64_t got = nod_read(file, buffer, want);
        if (got <= 0) {
            fail("nod_read");
            goto done;
        }
        if (fwrite(buffer, 1, (size_t)got, out) != (size_t)got) {
            perror("fwrite");
            goto done;
        }
        total += (uint32_t)got;
    }
    fprintf(stderr, "extracted %u bytes from %s\n", total, argv[2]);
    result = 0;

done:
    if (out != NULL) {
        fclose(out);
    }
    nod_free(file);
    nod_free(partition);
    nod_free(disc);
    return result;
}
