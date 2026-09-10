/* SPDX-License-Identifier: GPL-3.0-only */
/* V4-DISPLAY-01 no adapter: politica `stretch` declarada pelo port.
 *
 * O jogo desenha a 16:9 e faz letterbox por conta propria em qualquer
 * superficie de outro aspecto (barras pretas em cima e embaixo num painel
 * 4:3).  Com a politica `stretch`, a Unity passa a enxergar uma superficie
 * INTERNA 16:9 (largura do painel x 9/16), renderizada num FBO do adapter, e
 * o present estica esse alvo para o painel inteiro.  Quando o painel ja e'
 * 16:9 (Mali-450 1280x720) o plano e' no-op: nenhum FBO, nenhum byte muda. */
#ifndef ST_STRETCH_H
#define ST_STRETCH_H

/* Decide o plano para o drawable medido; devolve 1 quando o stretch esta
 * ativo (e escreve a resolucao interna), 0 quando e' no-op. */
int st_stretch_setup(int panel_w, int panel_h, int *internal_w, int *internal_h);
int st_stretch_active(void);
void st_stretch_internal_size(int *w, int *h);
void st_stretch_panel_size(int *w, int *h);
/* Mapeia a coordenada lógica 1280x720 para o retângulo realmente apresentado
 * (painel inteiro em stretch; conteúdo central em preserve). */
void st_stretch_logical_to_panel(float sx, float sy, float *dx, float *dy);
/* Ponto de ligacao do glBindFramebuffer(0): devolve o FBO que substitui o
 * default (cria-o na primeira chamada, com o contexto corrente).  0 quando o
 * stretch nao esta ativo ou o FBO nao pode ser criado. */
unsigned st_stretch_default_fbo(void);
/* Antes do swap: liga o framebuffer 0 real, viewport do painel e desenha o
 * alvo interno esticado.  Deixa o binding em 0 (a seta desenha por cima). */
void st_stretch_present(void);
/* Depois do swap: devolve o FBO interno como "default" para o proximo quadro. */
void st_stretch_after_present(void);

#endif
