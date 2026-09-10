/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * nx_aspect_policy -- TRADUÇÃO, não política (Tearscape 0.2.17, V5).
 *
 * Até a 0.2.16 este arquivo carregava a REGRA de aspect do jogo: o limiar de
 * ratio (<= 1,2 = letterbox), a precedência entre NEXTOSSETTINGS.txt e o
 * override.cfg do dono, o content rect. O `nxcompat_video` do framework já
 * implementava exatamente a mesma regra e este port não o referenciava —
 * duas autoridades para uma decisão só, e o próximo port Godot copiaria o
 * remendo (auditoria de 03/09, item E5).
 *
 * A decisão agora é UMA, do framework: `nxcompat_video_resolve_owner()`.
 * O que sobra aqui é a única coisa que é mesmo deste port — dizer como a
 * policy efetiva se escreve no vocabulário da Godot:
 *
 *   preserve -> "keep"     (letterbox, proporção mantida)
 *   stretch  -> "ignore"   (a chave Godot `ignore` SIGNIFICA stretch, 7A.3)
 *   engine   -> NULL       (herda: não escrever em ProjectSettings)
 *
 * `expand` e `keep_width` continuam proibidos porque a câmera do Tearscape
 * deriva o zoom da ALTURA do viewport lógico (Camera::InitZoom =
 * viewport.y/144) — restrição da engine, e por isso mora aqui e não no
 * framework. `crop` e `integer` são tokens /2 válidos que esta engine não
 * honra (viewport fixo 640x360); o adapter-env os traduz para auto com
 * diagnóstico antes de o engine ver.
 */
#ifndef NX_ASPECT_POLICY_H
#define NX_ASPECT_POLICY_H

#include "nxcompat_video.h"

#include <string.h>

#define NX_ASPECT_POLICY_SCHEMA "nx-aspect-policy/2"
#define NX_ASPECT_SHIPPED_DEFAULT "ignore" /* o valor que o jogo publica */

/* A tradução inteira. Cinco linhas. */
static inline const char *nx_godot_aspect(nxcompat_video_aspect effective) {
	if (effective == NXCOMPAT_VIDEO_ASPECT_PRESERVE) {
		return "keep";
	}
	if (effective == NXCOMPAT_VIDEO_ASPECT_STRETCH) {
		return "ignore";
	}
	return NULL; /* engine/inherit: o valor do projeto fica como está */
}

/* A inversa, para contar ao framework o que o override.cfg do dono diz. */
static inline const char *nx_godot_aspect_to_schema(const char *godot_value) {
	if (godot_value == NULL || godot_value[0] == '\0') {
		return NULL;
	}
	if (strcmp(godot_value, "keep") == 0) {
		return "preserve";
	}
	if (strcmp(godot_value, "ignore") == 0) {
		return "stretch";
	}
	return NULL; /* expand/keep_width/etc: fora do que este port aplica */
}

#endif /* NX_ASPECT_POLICY_H */
