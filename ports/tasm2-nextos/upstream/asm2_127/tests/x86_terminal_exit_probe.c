#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void terminal_probe_atexit(void)
{
    fputs("ASM2_X86_TERMINAL_PROBE_ATEXIT\n", stderr);
    fflush(stderr);
}

int main(int argc, char **argv)
{
    char *end = NULL;
    long requested_code;

    if (argc != 3 ||
        (strcmp(argv[1], "return") != 0 &&
         strcmp(argv[1], "_exit") != 0)) {
        fprintf(stderr,
                "usage: %s return|_exit exit-code\n",
                argc > 0 ? argv[0] : "x86_terminal_exit_probe");
        return 2;
    }

    errno = 0;
    requested_code = strtol(argv[2], &end, 10);
    if (errno != 0 || !end || *end != '\0' ||
        requested_code < 0 || requested_code > 255) {
        fprintf(stderr, "invalid exit code: %s\n", argv[2]);
        return 2;
    }

    if (atexit(terminal_probe_atexit) != 0) {
        fputs("ASM2_X86_TERMINAL_PROBE_ERROR atexit\n", stderr);
        return 3;
    }

    fprintf(stderr,
            "ASM2_X86_TERMINAL_PROBE_BEGIN mode=%s code=%ld\n",
            argv[1], requested_code);
    fflush(NULL);

    if (strcmp(argv[1], "_exit") == 0) {
        fprintf(stderr,
                "ASM2_X86_TERMINAL_PROBE_BOUNDARY mode=_exit code=%ld\n",
                requested_code);
        fflush(NULL);
        _exit((int)requested_code);
    }

    fprintf(stderr,
            "ASM2_X86_TERMINAL_PROBE_BOUNDARY mode=return code=%ld\n",
            requested_code);
    return (int)requested_code;
}
