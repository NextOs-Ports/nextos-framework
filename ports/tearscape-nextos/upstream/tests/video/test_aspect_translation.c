/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Tearscape 0.2.18 (V5): o port NÃO decide aspect — ele TRADUZ.
 *
 * Até a 0.2.16 o `nx_aspect_policy.h` carregava a regra de ratio, a
 * precedência e o content rect, duplicando o `nxcompat_video` (auditoria de
 * 03/09, item E5). Este gate prova as duas coisas que sobraram sendo do port:
 *   (a) a TRADUÇÃO nos dois sentidos (preserve<->keep, stretch<->ignore,
 *       engine->não escrever), e que ela NÃO inventa valor para os tokens
 *       que esta engine não aplica (expand/keep_width);
 *   (b) que a decisão vem da autoridade única, com o algoritmo `auto` que
 *       ESTE port declara, nos quatro drawables normativos + portrait.
 * Se alguém puser uma regra de ratio de volta no port, o mutante (b) morre:
 * a expectativa aqui é calculada pelo framework, não copiada.
 */
#include "nx_aspect_policy.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)

/* Exatamente o que o display server monta, sem a Godot no meio. */
static int resolve(const char *settings, const char *hook, const char *native,
		int native_present, int native_edited, int dw, int dh,
		nxcompat_video_owner_decision *out) {
	nxcompat_video_owner_input in;
	memset(&in, 0, sizeof in);
	in.settings_aspect = settings;
	in.port_env_aspect = hook;
	in.native_config_aspect = native;
	in.native_config_present = native_present;
	in.native_config_edited = native_edited;
	in.auto_algorithm_declared = 1;
	in.auto_algorithm_result = nxcompat_video_auto_stretch();
	in.package_default = NXCOMPAT_VIDEO_ASPECT_STRETCH;
	in.source_w = 640; in.source_h = 360;
	in.drawable_w = dw; in.drawable_h = dh;
	in.cas_generation = 1u;
	return nxcompat_video_resolve_owner(&in, out);
}

int main(void) {
	nxcompat_video_owner_decision d;

	/* (a) a tradução, que é tudo o que sobrou de política no port */
	CHECK(strcmp(nx_godot_aspect(NXCOMPAT_VIDEO_ASPECT_PRESERVE), "keep") == 0,
			"preserve traduz para keep");
	CHECK(strcmp(nx_godot_aspect(NXCOMPAT_VIDEO_ASPECT_STRETCH), "ignore") == 0,
			"stretch traduz para ignore (a chave Godot `ignore` SIGNIFICA stretch)");
	CHECK(nx_godot_aspect(NXCOMPAT_VIDEO_ASPECT_ENGINE) == NULL,
			"engine/inherit nao escreve em ProjectSettings");
	CHECK(nx_godot_aspect(NXCOMPAT_VIDEO_ASPECT_CROP) == NULL &&
			nx_godot_aspect(NXCOMPAT_VIDEO_ASPECT_INTEGER) == NULL,
			"crop/integer: esta engine nao aplica, entao nao ha traducao inventada");
	CHECK(strcmp(nx_godot_aspect_to_schema("keep"), "preserve") == 0 &&
			strcmp(nx_godot_aspect_to_schema("ignore"), "stretch") == 0,
			"a inversa conta ao framework o que o override.cfg do dono diz");
	CHECK(nx_godot_aspect_to_schema("expand") == NULL &&
			nx_godot_aspect_to_schema("keep_width") == NULL &&
			nx_godot_aspect_to_schema("") == NULL &&
			nx_godot_aspect_to_schema(NULL) == NULL,
			"MUTANTE morto: expand/keep_width nunca viram policy (a camera segue a ALTURA do viewport)");

	/* (b) a decisao vem da autoridade unica, nos drawables normativos */
	CHECK(resolve(NULL, NULL, NULL, 0, 0, 720, 720, &d) == 0 &&
			d.geometry.effective == NXCOMPAT_VIDEO_ASPECT_STRETCH &&
			d.geometry.content.w == 720 && d.geometry.content.h == 720 &&
			d.geometry.content.y == 0 && d.geometry.bar_top == 0 &&
			d.geometry.bar_bottom == 0 &&
			strcmp(nx_godot_aspect(d.geometry.effective), "ignore") == 0,
			"painel 1:1 720x720 sob auto: stretch ocupa o painel inteiro, Godot `ignore`");
	CHECK(resolve(NULL, NULL, NULL, 0, 0, 640, 480, &d) == 0 &&
			d.geometry.effective == NXCOMPAT_VIDEO_ASPECT_STRETCH &&
			strcmp(nx_godot_aspect(d.geometry.effective), "ignore") == 0,
			"painel 4:3 640x480 sob auto: stretch (o preenchimento provado no R2), Godot `ignore`");
	CHECK(resolve(NULL, NULL, NULL, 0, 0, 1280, 720, &d) == 0 &&
			d.geometry.effective == NXCOMPAT_VIDEO_ASPECT_STRETCH && !d.geometry.distorted,
			"painel 16:9 1280x720 sob auto: stretch e no-op geometrico");
	CHECK(resolve(NULL, NULL, NULL, 0, 0, 1920, 1080, &d) == 0 &&
			d.geometry.effective == NXCOMPAT_VIDEO_ASPECT_STRETCH && !d.geometry.distorted,
			"painel 16:9 1920x1080 sob auto: stretch e no-op geometrico");
	CHECK(resolve(NULL, NULL, NULL, 0, 0, 720, 1280, &d) == 0 &&
			d.geometry.effective == NXCOMPAT_VIDEO_ASPECT_STRETCH &&
			d.geometry.content.w == 720 && d.geometry.content.h == 1280,
			"portrait 720x1280 sob auto: stretch ocupa o drawable inteiro");
	CHECK(resolve("preserve", NULL, NULL, 0, 0, 720, 720, &d) == 0 &&
			d.source == NXCOMPAT_VIDEO_SOURCE_SETTINGS &&
			d.geometry.effective == NXCOMPAT_VIDEO_ASPECT_PRESERVE &&
			d.geometry.content.w == 720 && d.geometry.content.h == 405 &&
			d.geometry.content.y == 157 && d.geometry.bar_top == 157 &&
			d.geometry.bar_bottom == 158,
			"dono de 720x720 escolhe preserve: 16:9 centralizado com barras 157/158");

	/* precedencia: quem manda e o framework, e o port so relata as fontes */
	CHECK(resolve("preserve", NULL, NULL, 0, 0, 640, 480, &d) == 0 &&
			d.source == NXCOMPAT_VIDEO_SOURCE_SETTINGS &&
			d.geometry.effective == NXCOMPAT_VIDEO_ASPECT_PRESERVE,
			"NEXTOSSETTINGS.txt vence o auto num painel 4:3");
	CHECK(resolve("preserve", "stretch", NULL, 0, 0, 640, 480, &d) == 0 &&
			d.source == NXCOMPAT_VIDEO_SOURCE_PORT_ENV &&
			d.geometry.effective == NXCOMPAT_VIDEO_ASPECT_STRETCH,
			"port-env.sh do dono e a ultima palavra (o hook e lido depois do arquivo tipado)");
	CHECK(resolve(NULL, NULL, "preserve", 1, 1, 640, 480, &d) == 0 &&
			d.source == NXCOMPAT_VIDEO_SOURCE_NATIVE_CONFIG &&
			d.geometry.effective == NXCOMPAT_VIDEO_ASPECT_PRESERVE,
			"override.cfg EDITADO pelo dono vence o auto");
	CHECK(resolve(NULL, NULL, "stretch", 1, 0, 720, 720, &d) == 0 &&
			d.source == NXCOMPAT_VIDEO_SOURCE_AUTO &&
			d.geometry.effective == NXCOMPAT_VIDEO_ASPECT_STRETCH,
			"MUTANTE morto: override.cfg INTOCADO e default de pacote e NAO vence o auto");
	CHECK(resolve("keep", NULL, NULL, 0, 0, 720, 720, &d) == -1,
			"token nativo da engine (`keep`) no arquivo tipado e recusado: o schema so fala o schema");

	/* readback: sob `engine` o port NAO afirma nada, entao ler o valor do
	 * projeto como se fosse medicao da nossa decisao produz mismatch falso
	 * toda vez que o projeto publicado diz "ignore". */
	{
		nxcompat_video_readback rb;
		char line[512];
		nxcompat_video_owner_input in;
		memset(&in, 0, sizeof in);
		in.settings_authority = "engine";
		in.native_config_present = 1;
		in.native_config_aspect = "stretch";
		in.source_w = 640; in.source_h = 360;
		in.drawable_w = 640; in.drawable_h = 480;
		in.cas_generation = 1u;
		in.package_default = NXCOMPAT_VIDEO_ASPECT_STRETCH;
		CHECK(nxcompat_video_resolve_owner(&in, &d) == 0 &&
				d.geometry.effective == NXCOMPAT_VIDEO_ASPECT_ENGINE,
				"authority=engine: a decisao eleita e inherit");
		memset(&rb, 0, sizeof rb);
		rb.api_version = NXCOMPAT_VIDEO_OWNER_API_VERSION;
		rb.drawable_w = 640; rb.drawable_h = 480; rb.cas_generation = 1u;
		rb.effective = d.geometry.effective;
		rb.content = d.geometry.content;
		CHECK(nxcompat_video_readback_check(&d, &rb, line, sizeof line) == 0,
				"readback de inherit bate quando o adapter NAO le o valor do projeto como medicao");
		rb.effective = NXCOMPAT_VIDEO_ASPECT_STRETCH;
		CHECK(nxcompat_video_readback_check(&d, &rb, line, sizeof line) == -1,
				"MUTANTE morto: ler o `ignore` do projeto sob inherit daria mismatch FALSO");
	}

	printf(fails ? "tearscape-aspect-translation: FAIL\n" : "tearscape-aspect-translation: OK\n");
	return fails ? 1 : 0;
}
