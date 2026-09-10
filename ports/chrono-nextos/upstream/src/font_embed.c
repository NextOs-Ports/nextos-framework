/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * font_embed.c -- Noto Sans Regular (SIL OFL 1.1, fonts/OFL.txt) embutida no
 * proprio executavel.
 *
 * Por que: o pacote 1.1.1 nao embalava fonts/ e o menu ficou SEM TEXTO no
 * muOS (RG40XX-H), que nao traz nenhuma das fontes de firmware da lista de
 * deteccao. O texto da UI nao pode depender de arquivo ao lado do binario nem
 * do firmware: a fonte vive dentro do ELF e e' o piso garantido, seja qual for
 * o CFW ou o APK. Os arquivos em fonts/ continuam sendo distribuidos (o
 * usuario pode trocar por CHRONO_FONT ou pela pasta) e tem prioridade.
 */
__asm__(
    ".section .rodata\n"
    ".balign 16\n"
    ".globl chrono_embedded_font\n"
    "chrono_embedded_font:\n"
    ".incbin \"fonts/NotoSans-Regular.ttf\"\n"
    ".globl chrono_embedded_font_end\n"
    "chrono_embedded_font_end:\n"
    ".byte 0\n"
    ".previous\n");
