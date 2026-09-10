/* Traducao de GLSL ES 3.00 (o que a Unity gera) para GLSL ES 1.00 (o que o
 * Mali-450/Utgard compila).  Ver essl1.c para o porque. */
#ifndef SF_ESSL1_H
#define SF_ESSL1_H

#include <stddef.h>

enum {
    SF_ESSL1_REASON_VERSION_300 = 1u << 0,
    SF_ESSL1_REASON_STORAGE = 1u << 1,
    SF_ESSL1_REASON_UNITY_ALIASES = 1u << 2,
    SF_ESSL1_REASON_UNITY_ALIAS_PARTIAL = 1u << 3,
};

/* Classifica por que uma fonte precisa da ponte ESSL100. O mesmo classificador
 * aplicado na saida precisa devolver zero; isso impede uma traducao parcial de
 * ser anunciada como sucesso. */
unsigned sf_essl1_classify(const char *src, size_t len);

/* Devolve buffer novo (free pelo chamador) com a fonte traduzida, ou NULL se
 * nada precisou mudar.  is_fragment separa as regras dos dois estagios. */
char *sf_essl1_translate(const char *src, size_t len, int is_fragment,
                         size_t *out_len);

int sf_essl1_disabled(void);

#endif
