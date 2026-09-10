// SPDX-License-Identifier: GPL-3.0-only
/*
 * Tiny freestanding ARMHF probe for the optional Spruce PC environment.
 * It asks the target SDL library which video drivers were compiled in without
 * initializing video, DRM, Mali, framebuffer, audio or input.
 */

struct sdl_version {
    unsigned char major;
    unsigned char minor;
    unsigned char patch;
};

extern int SDL_GetNumVideoDrivers(void);
extern const char *SDL_GetVideoDriver(int index);
extern void SDL_GetVersion(struct sdl_version *version);

static unsigned int text_length(const char *text)
{
    unsigned int length = 0;

    while (text[length] != '\0')
        ++length;
    return length;
}

static void write_text(const char *text)
{
    register long r0 __asm__("r0") = 1;
    register const char *r1 __asm__("r1") = text;
    register unsigned long r2 __asm__("r2") = text_length(text);
    register long r7 __asm__("r7") = 4;

    __asm__ volatile("svc 0"
                     : "+r"(r0)
                     : "r"(r1), "r"(r2), "r"(r7)
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
    register long r0 __asm__("r0") = status;
    register long r7 __asm__("r7") = 1;

    __asm__ volatile("svc 0"
                     : "+r"(r0)
                     : "r"(r7)
                     : "memory");
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
    for (index = 0; index < count; ++index) {
        const char *driver = SDL_GetVideoDriver(index);

        if (index != 0)
            write_text(",");
        if (driver != (const char *)0)
            write_text(driver);
    }
    write_text("\n");

    exit_process(count > 0 ? 0 : 1);
}
