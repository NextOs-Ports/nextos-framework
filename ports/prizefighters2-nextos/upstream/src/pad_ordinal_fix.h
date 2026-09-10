/*
 * pad_ordinal_fix.h -- NextOS positional gamepad normalisation.
 *
 * Old 3.14/Amlogic kernels using hid-generic can expose HID buttons by report
 * order instead of their modern semantic Linux key codes.  BTN_C/BTN_Z in the
 * evdev bitmap identify that layout.  ControllerDB entries created on modern
 * kernels (or from Nintendo-style printed labels) then swap A/B and X/Y.
 *
 * This proven NextOS fix detects only that legacy signature and installs an
 * SDL mapping from the physical order of the controller class.  Modern pads
 * keep their existing semantic mapping untouched.  A manual <PREFIX>_PAD_MAP
 * always wins; <PREFIX>_ORDINAL_FIX=0|off disables detection.
 */
#ifndef PAD_ORDINAL_FIX_H
#define PAD_ORDINAL_FIX_H

#include <fcntl.h>
#include <linux/input.h>
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define PAD_ORD_NBITS(x) (((x) + 8 * sizeof(long) - 1) / (8 * sizeof(long)))

static int pad_ord_test_bit(const unsigned long *bits, int index)
{
    return !!((bits[index / (8 * sizeof(long))] >>
               (index % (8 * sizeof(long)))) & 1);
}

static int pad_ord_abs_rank(const unsigned long *abs_bits, int code)
{
    if (!pad_ord_test_bit(abs_bits, code))
        return -1;
    int rank = 0;
    for (int i = 0; i < code; i++) {
        if (i >= ABS_HAT0X && i <= ABS_HAT3Y)
            continue;
        if (pad_ord_test_bit(abs_bits, i))
            rank++;
    }
    return rank;
}

static void pad_ordinal_fix_apply(int index, const char *env_prefix)
{
    char env_name[64];
    snprintf(env_name, sizeof env_name, "%s_ORDINAL_FIX", env_prefix);
    const char *env = getenv(env_name);
    if (env && (!strcmp(env, "0") || !strcasecmp(env, "off")))
        return;
    snprintf(env_name, sizeof env_name, "%s_PAD_MAP", env_prefix);
    const char *user_mapping = getenv(env_name);
    if (user_mapping && *user_mapping)
        return;

    SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(index);
    int vendor = guid.data[4] | (guid.data[5] << 8);
    int product = guid.data[8] | (guid.data[9] << 8);
    if (!vendor && !product)
        return;

    unsigned long key_bits[PAD_ORD_NBITS(KEY_MAX + 1)];
    unsigned long abs_bits[PAD_ORD_NBITS(ABS_MAX + 1)];
    int found = 0;
    for (int i = 0; i < 32 && !found; i++) {
        char path[64];
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        struct input_id id;
        memset(&id, 0, sizeof id);
        memset(key_bits, 0, sizeof key_bits);
        memset(abs_bits, 0, sizeof abs_bits);
        if (ioctl(fd, EVIOCGID, &id) == 0 &&
            id.vendor == vendor && id.product == product &&
            ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key_bits), key_bits) >= 0 &&
            ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs_bits), abs_bits) >= 0 &&
            pad_ord_test_bit(key_bits, BTN_GAMEPAD) &&
            (pad_ord_test_bit(key_bits, BTN_C) ||
             pad_ord_test_bit(key_bits, BTN_Z)))
            found = 1;
        close(fd);
    }
    if (!found)
        return;

    char guid_text[64];
    SDL_JoystickGetGUIDString(guid, guid_text, sizeof guid_text);
    const char *device_name = SDL_JoystickNameForIndex(index);
    char safe_name[64];
    snprintf(safe_name, sizeof safe_name, "%s",
             device_name && *device_name ? device_name : "pad");
    for (char *c = safe_name; *c; c++)
        if (*c == ',' || *c == ':')
            *c = ' ';

    int sony = vendor == 0x054c;
    const char *buttons = sony
        ? "x:b0,a:b1,b:b2,y:b3,leftshoulder:b4,rightshoulder:b5,"
          "lefttrigger:b6,righttrigger:b7,back:b8,start:b9,leftstick:b10,"
          "rightstick:b11,guide:b12,"
        : "a:b0,b:b1,x:b3,y:b4,leftshoulder:b6,rightshoulder:b7,"
          "lefttrigger:b8,righttrigger:b9,back:b10,start:b11,guide:b12,"
          "leftstick:b13,rightstick:b14,";

    int left_x = pad_ord_abs_rank(abs_bits, ABS_X);
    int left_y = pad_ord_abs_rank(abs_bits, ABS_Y);
    int right_x = pad_ord_abs_rank(abs_bits, ABS_Z);
    int right_y = pad_ord_abs_rank(abs_bits, ABS_RZ);
    if (right_x < 0 || right_y < 0) {
        right_x = pad_ord_abs_rank(abs_bits, ABS_RX);
        right_y = pad_ord_abs_rank(abs_bits, ABS_RY);
    }

    char mapping[512];
    int used = snprintf(mapping, sizeof mapping, "%s,%s,platform:Linux,%s",
                        guid_text, safe_name, buttons);
    if (left_x >= 0)
        used += snprintf(mapping + used, sizeof mapping - (size_t)used,
                         "leftx:a%d,", left_x);
    if (left_y >= 0)
        used += snprintf(mapping + used, sizeof mapping - (size_t)used,
                         "lefty:a%d,", left_y);
    if (right_x >= 0)
        used += snprintf(mapping + used, sizeof mapping - (size_t)used,
                         "rightx:a%d,", right_x);
    if (right_y >= 0)
        used += snprintf(mapping + used, sizeof mapping - (size_t)used,
                         "righty:a%d,", right_y);
    if (pad_ord_test_bit(abs_bits, ABS_HAT0X) &&
        pad_ord_test_bit(abs_bits, ABS_HAT0Y))
        snprintf(mapping + used, sizeof mapping - (size_t)used,
                 "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,");

    int result = SDL_GameControllerAddMapping(mapping);
    fprintf(stderr,
            "[pad] ordinal fix (%s) vid=%04x pid=%04x result=%d: %s\n",
            sony ? "sony" : "hid", vendor, product, result, mapping);
}

#endif /* PAD_ORDINAL_FIX_H */
