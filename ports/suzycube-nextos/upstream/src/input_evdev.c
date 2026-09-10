/* Pure evdev helpers shared by the runtime adapter and its host unit test. */
#include "input_evdev.h"

int sc_evdev_test_bit(const unsigned long *bits, size_t words, int code)
{
    if (!bits || code < 0 || code > KEY_MAX)
        return 0;
    size_t word = (size_t)code / SC_EVDEV_WORD_BITS;
    if (word >= words)
        return 0;
    return (int)((bits[word] >> ((size_t)code % SC_EVDEV_WORD_BITS)) & 1UL);
}

/* SDL's Linux joystick backend numbers EV_KEY buttons in two passes: gamepad
 * codes first, then every lower EV_KEY code -- but that order is NOT portable
 * (muOS numbers the non-gamepad keys first, which shifts every b<N>).  So the
 * reconstruction is used only where a disagreement is harmless: the face
 * classifier below bails out to "unknown" and changes nothing.  Never derive
 * a keycode from an SDL ordinal to read button state. */
static int sc_evdev_button_index(const unsigned long *bits, size_t words,
                                 int wanted_code)
{
    int index = 0;
    for (int code = BTN_JOYSTICK; code < KEY_MAX; code++) {
        if (!sc_evdev_test_bit(bits, words, code))
            continue;
        if (code == wanted_code)
            return index;
        index++;
    }
    for (int code = 0; code < BTN_JOYSTICK; code++) {
        if (!sc_evdev_test_bit(bits, words, code))
            continue;
        if (code == wanted_code)
            return index;
        index++;
    }
    return -1;
}

int sc_evdev_sdl_button_index(const unsigned long *bits, size_t words,
                              int code)
{
    if (!sc_evdev_test_bit(bits, words, code))
        return -1;
    return sc_evdev_button_index(bits, words, code);
}

/* Ao contrario dos botoes -- cuja numeracao depende da ordem de varredura do
 * SDL e por isso NAO e' portavel entre firmwares --, o eixo de indice N e' o
 * N-esimo codigo EV_ABS declarado em ordem crescente, pulando ABS_HAT0..3
 * (que o SDL publica como hat, nao como eixo).  Essa regra e' estavel, e e'
 * o que liga um bind "leftx:a0" do mapping ao codigo ABS de verdade. */
int sc_evdev_abs_code_for_sdl_axis(const unsigned long *abs_bits, size_t words,
                                   int axis_index)
{
    if (!abs_bits || axis_index < 0)
        return -1;
    int index = 0;
    for (int code = 0; code <= ABS_MAX; code++) {
        if (code >= ABS_HAT0X && code <= ABS_HAT3Y)
            continue;
        if (!sc_evdev_test_bit(abs_bits, words, code))
            continue;
        if (index == axis_index)
            return code;
        index++;
    }
    return -1;
}

/* Guarda antitravamento: o unico veredito que o snapshot do kernel pode dar
 * sem conhecer a numeracao do firmware e' "nada esta' pressionado".  Quando
 * ele diz isso e o SDL ainda acha que tem botao preso (release perdido pelo
 * CFW), o estado do SDL esta' errado e e' zerado. */
int sc_evdev_any_key_pressed(const unsigned long *state,
                             const unsigned long *capabilities, size_t words)
{
    if (!state || !capabilities)
        return 1;
    for (size_t i = 0; i < words; i++)
        if (state[i] & capabilities[i])
            return 1;
    return 0;
}

/* Um dpad declarado como eixo ABS tem range minusculo (-1..1, 0..2); um stick
 * de verdade reporta dezenas de posicoes (0..255 e' o menor visto em campo).
 * Aceitar o range curto como analogico e' o que faz o personagem "andar
 * sozinho": o repouso do eixo degenerado normaliza longe do centro. */
int sc_evdev_axis_is_analog(int minimum, int maximum)
{
    return maximum > minimum &&
           (long long)maximum - (long long)minimum >= 16;
}

enum sc_evdev_face_mapping sc_evdev_classify_face_mapping(
    const unsigned long *bits, size_t words,
    int bind_a, int bind_b, int bind_x, int bind_y)
{
    const int south = sc_evdev_sdl_button_index(bits, words, BTN_SOUTH);
    const int east = sc_evdev_sdl_button_index(bits, words, BTN_EAST);
    const int north = sc_evdev_sdl_button_index(bits, words, BTN_NORTH);
    const int west = sc_evdev_sdl_button_index(bits, words, BTN_WEST);

    if (south < 0 || east < 0 || north < 0 || west < 0)
        return SC_EVDEV_FACE_UNKNOWN;

    /* Xbox/InControl position semantics: A south, B east, X west, Y north. */
    if (bind_a == south && bind_b == east &&
        bind_x == west && bind_y == north)
        return SC_EVDEV_FACE_XBOX_POSITIONAL;

    /* Common R36S mapping follows the printed Nintendo-style letters instead:
     * A east, B south, X north, Y west.  The game's fixed Xbox glyph profile
     * consequently presents both pairs reversed unless we normalize it. */
    if (bind_a == east && bind_b == south &&
        bind_x == north && bind_y == west)
        return SC_EVDEV_FACE_NINTENDO_LABELS;

    return SC_EVDEV_FACE_UNKNOWN;
}
