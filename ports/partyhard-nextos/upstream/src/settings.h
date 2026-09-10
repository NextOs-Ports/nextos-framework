/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ST_SETTINGS_H
#define ST_SETTINGS_H

#include "nxcompat_settings.h"

/* Snapshot único do NEXTOSSETTINGS.txt do dono. Arquivo ausente ou inválido
 * usa a semente imutável do pacote sem reescrever os bytes do usuário. */
const nxcompat_settings *st_settings_get(void);
const char *st_settings_source(void);

#endif
