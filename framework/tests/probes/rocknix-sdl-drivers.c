// SPDX-License-Identifier: GPL-3.0-only
/*
 * Freestanding AArch64 probe for the optional ROCKNIX PC environment.
 * It only queries metadata exported by the firmware's own SDL library.  No
 * subsystem, display server, DRM node, GPU, audio device or input is opened.
 */

struct sdl_version {
    unsigned char major;
    unsigned char minor;
    unsigned char patch;
};

extern int SDL_GetNumVideoDrivers(void);
extern const char *SDL_GetVideoDriver(int index);
extern void SDL_GetVersion(struct sdl_version *version);

static unsigned long text_length(const char *text)
{
    unsigned long length = 0;

    while (text[length] != '\0')
        ++length;
    return length;
}

static void write_text(const char *text)
{
    register long x0 __asm__("x0") = 1;
    register const char *x1 __asm__("x1") = text;
    register unsigned long x2 __asm__("x2") = text_length(text);
    register long x8 __asm__("x8") = 64;

    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x8)
                     : "memory");
}

static void write_decimal(unsigned int value)
{
    char output[2];
    unsigned int hundreds = 0;
    unsigned int tens = 0;

    while (value >= 100) {
        value -= 100;
        ++hundreds;
    }
    while (value >= 10) {
        value -= 10;
        ++tens;
    }

    output[1] = '\0';
    if (hundreds != 0) {
        output[0] = (char)('0' + hundreds);
        write_text(output);
    }
    if (hundreds != 0 || tens != 0) {
        output[0] = (char)('0' + tens);
        write_text(output);
    }
    output[0] = (char)('0' + value);
    write_text(output);
}

static void exit_process(int status)
{
    register long x0 __asm__("x0") = status;
    register long x8 __asm__("x8") = 93;

    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    for (;;)
        ;
}

void _start(void)
{
    struct sdl_version version;
    int count;
    int index;

    version.major = 0;
    version.minor = 0;
    version.patch = 0;
    SDL_GetVersion(&version);
    write_text("sdl=");
    write_decimal(version.major);
    write_text(".");
    write_decimal(version.minor);
    write_text(".");
    write_decimal(version.patch);
    write_text("\ndrivers=");

    count = SDL_GetNumVideoDrivers();
    if (count <= 0 || count > 64) {
        write_text("<invalid>\n");
        exit_process(1);
    }
    for (index = 0; index < count; ++index) {
        const char *driver = SDL_GetVideoDriver(index);

        if (index != 0)
            write_text(",");
        if (driver == (const char *)0) {
            write_text("<null>\n");
            exit_process(1);
        }
        write_text(driver);
    }
    write_text("\n");
    exit_process(0);
}
