/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SUZYCUBE_INPUT_POLICY_H
#define SUZYCUBE_INPUT_POLICY_H

/* Cheap handheld sticks can rest well outside SDL's nominal center.  The
 * separate thresholds keep an active stick stable while still requiring a
 * deliberate movement to leave neutral. */
#define SC_INPUT_STICK_ENTER_DEADZONE 0.40f
#define SC_INPUT_STICK_EXIT_DEADZONE 0.30f
#define SC_INPUT_TRIGGER_DEADZONE 0.05f

/* O D-pad chega ao perfil Android/InControl como AXIS_HAT_X/Y de nivel. Cada
 * direcao tem amplitude inteira e as diagonais preservam os dois componentes;
 * o proprio InControl normaliza o vetor depois. Manter isto compartilhado com
 * o teste impede que um quadrante receba ganho diferente por engano. */
static inline void sc_input_dpad_axes(int up, int down, int left, int right,
                                      float *x, float *y)
{
    if (x)
        *x = (right ? 1.0f : 0.0f) - (left ? 1.0f : 0.0f);
    if (y)
        *y = (down ? 1.0f : 0.0f) - (up ? 1.0f : 0.0f);
}

#endif
