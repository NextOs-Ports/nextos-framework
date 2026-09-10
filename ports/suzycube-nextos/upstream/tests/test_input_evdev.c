#include "input_evdev.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void set_bit(unsigned long *bits, int code)
{
    bits[(size_t)code / SC_EVDEV_WORD_BITS] |=
        1UL << ((size_t)code % SC_EVDEV_WORD_BITS);
}

int main(void)
{
    unsigned long bits[SC_EVDEV_KEY_WORDS];
    memset(bits, 0, sizeof bits);

    const int codes[] = {
        BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST,
        BTN_TL, BTN_TR,
        BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT,
        BTN_TRIGGER_HAPPY1, BTN_TRIGGER_HAPPY2,
    };
    for (size_t i = 0; i < sizeof codes / sizeof *codes; i++)
        set_bit(bits, codes[i]);
    /* Lower EV_KEY codes are numbered in SDL's second pass, in code order. */
    set_bit(bits, KEY_ENTER);
    set_bit(bits, BTN_0);

    assert(sc_evdev_sdl_button_index(bits, SC_EVDEV_KEY_WORDS,
                                     BTN_SOUTH) == 0);
    assert(sc_evdev_sdl_button_index(bits, SC_EVDEV_KEY_WORDS,
                                     BTN_EAST) == 1);
    assert(sc_evdev_sdl_button_index(bits, SC_EVDEV_KEY_WORDS,
                                     BTN_NORTH) == 2);
    assert(sc_evdev_sdl_button_index(bits, SC_EVDEV_KEY_WORDS,
                                     BTN_WEST) == 3);
    assert(sc_evdev_sdl_button_index(bits, SC_EVDEV_KEY_WORDS,
                                     BTN_TRIGGER_HAPPY2) == 11);
    assert(sc_evdev_sdl_button_index(bits, SC_EVDEV_KEY_WORDS,
                                     KEY_ENTER) == 12);
    assert(sc_evdev_sdl_button_index(bits, SC_EVDEV_KEY_WORDS, BTN_0) == 13);

    assert(sc_evdev_classify_face_mapping(
               bits, SC_EVDEV_KEY_WORDS, 0, 1, 3, 2) ==
           SC_EVDEV_FACE_XBOX_POSITIONAL);
    assert(sc_evdev_classify_face_mapping(
               bits, SC_EVDEV_KEY_WORDS, 1, 0, 2, 3) ==
           SC_EVDEV_FACE_NINTENDO_LABELS);
    assert(sc_evdev_classify_face_mapping(
               bits, SC_EVDEV_KEY_WORDS, 0, 1, 2, 3) ==
           SC_EVDEV_FACE_UNKNOWN);

    /* Guarda antitravamento: o kernel so' pode dizer "nada pressionado",
     * e' esse veredito que cura o botao preso por release perdido. */
    unsigned long state[SC_EVDEV_KEY_WORDS];
    memset(state, 0, sizeof state);
    assert(sc_evdev_any_key_pressed(state, bits, SC_EVDEV_KEY_WORDS) == 0);
    set_bit(state, BTN_SOUTH);
    assert(sc_evdev_any_key_pressed(state, bits, SC_EVDEV_KEY_WORDS) == 1);
    /* Tecla fora das capacidades declaradas nao segura a guarda. */
    memset(state, 0, sizeof state);
    set_bit(state, KEY_POWER);
    assert(sc_evdev_any_key_pressed(state, bits, SC_EVDEV_KEY_WORDS) == 0);

    /* Eixo do mapping -> codigo ABS real.  Caso RG40XX-H/muOS: o aparelho
     * NAO declara ABS_X/Y, entao "leftx:a0" e' ABS_RX -- procurar ABS_X para
     * o stick esquerdo (v1.1.5/v1.1.6) deixava o stick morto. */
    unsigned long abs_bits[SC_EVDEV_ABS_WORDS];
    memset(abs_bits, 0, sizeof abs_bits);
    set_bit(abs_bits, ABS_RX);
    set_bit(abs_bits, ABS_RY);
    set_bit(abs_bits, ABS_RZ);
    set_bit(abs_bits, ABS_THROTTLE);
    set_bit(abs_bits, ABS_HAT0X);
    set_bit(abs_bits, ABS_HAT0Y);
    assert(sc_evdev_abs_code_for_sdl_axis(abs_bits, SC_EVDEV_ABS_WORDS, 0) ==
           ABS_RX);
    assert(sc_evdev_abs_code_for_sdl_axis(abs_bits, SC_EVDEV_ABS_WORDS, 1) ==
           ABS_RY);
    /* Ordem crescente de codigo: ABS_RZ (0x05) antes de ABS_THROTTLE (0x06). */
    assert(sc_evdev_abs_code_for_sdl_axis(abs_bits, SC_EVDEV_ABS_WORDS, 2) ==
           ABS_RZ);
    assert(sc_evdev_abs_code_for_sdl_axis(abs_bits, SC_EVDEV_ABS_WORDS, 3) ==
           ABS_THROTTLE);
    /* O hat nunca conta como eixo: e' hat no SDL. */
    assert(sc_evdev_abs_code_for_sdl_axis(abs_bits, SC_EVDEV_ABS_WORDS, 4) ==
           -1);
    assert(sc_evdev_abs_code_for_sdl_axis(abs_bits, SC_EVDEV_ABS_WORDS, -1) ==
           -1);

    /* Aparelho classico (dois sticks completos) nao muda de resultado. */
    unsigned long full[SC_EVDEV_ABS_WORDS];
    memset(full, 0, sizeof full);
    set_bit(full, ABS_X);
    set_bit(full, ABS_Y);
    set_bit(full, ABS_RX);
    set_bit(full, ABS_RY);
    assert(sc_evdev_abs_code_for_sdl_axis(full, SC_EVDEV_ABS_WORDS, 0) ==
           ABS_X);
    assert(sc_evdev_abs_code_for_sdl_axis(full, SC_EVDEV_ABS_WORDS, 3) ==
           ABS_RY);

    assert(sc_evdev_test_bit(bits, SC_EVDEV_KEY_WORDS, -1) == 0);
    assert(sc_evdev_test_bit(bits, SC_EVDEV_KEY_WORDS, KEY_MAX + 1) == 0);

    /* Eixo analogico de verdade tem range largo; dpad-como-ABS nao. */
    assert(sc_evdev_axis_is_analog(0, 255) == 1);
    assert(sc_evdev_axis_is_analog(-2048, 2047) == 1);
    assert(sc_evdev_axis_is_analog(-32768, 32767) == 1);
    assert(sc_evdev_axis_is_analog(-1, 1) == 0);   /* dpad declarado ABS */
    assert(sc_evdev_axis_is_analog(0, 1) == 0);    /* eixo degenerado */
    assert(sc_evdev_axis_is_analog(0, 2) == 0);
    assert(sc_evdev_axis_is_analog(0, 0) == 0);
    assert(sc_evdev_axis_is_analog(10, 0) == 0);   /* range invertido */

    puts("SUZY CUBE EVDEV UNIT: PASS");
    return 0;
}
