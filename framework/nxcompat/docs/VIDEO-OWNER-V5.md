# nxcompat_video — a autoridade ÚNICA de aspect/resolução (V5, 0.5.3)

Regra do NextOS de 03/09/2026: **configuração de tela (aspect, resolução,
content rect, touch) nunca se corrige dentro de um port.** A decisão nasce
aqui, com teste e mutante, e vale para todo jogo e todo aparelho. No port
sobra apenas o **adaptador da engine**: a cola que traduz a policy efetiva
para o vocabulário daquela engine.

## Por que isto existe

O Tearscape 0.2.16/0.2.17 carregava `nx_aspect_policy.h` dentro do patch da
Godot, com a mesma regra de ratio (`≤ 1,2 → letterbox`) que o
`nxcompat_video` já implementava em `nxcompat_video_auto_ratio_threshold`, e
zero referência a este componente. Duas autoridades para a mesma decisão: o
próximo port Godot copiaria o remendo e uma correção futura teria de ser
feita duas vezes. (Auditoria de 03/09, item E5.)

## A chamada única

```c
nxcompat_video_owner_input in;
nxcompat_video_owner_decision out;

memset(&in, 0, sizeof in);
in.settings_authority   = settings.video_authority;   /* "" = nextos */
in.settings_aspect      = settings.video_aspect;      /* "" = ausente */
in.port_env_aspect      = getenv_allowlisted("NX_VIDEO_ASPECT");
in.native_config_aspect = engine_config_translated;   /* já no schema */
in.native_config_present = have_native_config;
in.native_config_edited  = native_config_differs_from_seed;
in.auto_algorithm_declared = 1;                       /* este port declara UM */
in.auto_algorithm_result   = nxcompat_video_auto_stretch();
in.package_default = NXCOMPAT_VIDEO_ASPECT_STRETCH;
in.source_w = 1280; in.source_h = 720;
in.drawable_w = drawable_w; in.drawable_h = drawable_h;
in.cas_generation = video_config_generation;          /* exigido por synchronized */

if (nxcompat_video_resolve_owner(&in, &out) != 0)
        fail_before_init(out.reason);                 /* nunca last-writer-wins */
```

Na 0.5.3, `auto-stretch` formaliza o caso em que o default histórico aprovado
é preencher qualquer painel. Ele não tenta adivinhar a preferência pelo formato
da tela: em um painel 720×720 continua `stretch`; o dono seleciona
`video.aspect=preserve` quando quiser 16:9 com barras. `ratio-threshold` e
`epsilon` continuam disponíveis, sem mudança, para ports que os declararam.

`out.geometry` traz o content rect, as barras e a inversa para touch/cursor;
`out.source` diz **quem ganhou**; `nxcompat_video_owner_receipt()` emite o
`NX-VIDEO/1` com authority + fonte.

## O que fica no port: a tradução (Godot, cinco linhas)

```c
/* nx_aspect_policy.h — tradução, não política. */
static inline const char *nx_godot_aspect(nxcompat_video_aspect effective) {
        if (effective == NXCOMPAT_VIDEO_ASPECT_PRESERVE) return "keep";
        if (effective == NXCOMPAT_VIDEO_ASPECT_STRETCH)  return "ignore";
        return NULL; /* engine/inherit: não escrever em ProjectSettings */
}
```

A chave Godot `aspect="ignore"` **significa stretch** — não renomear como
preserve (7A.3). `expand`/`keep_width` continuam proibidos no Tearscape porque
a câmera deriva o zoom da altura do viewport lógico; isso é uma restrição da
engine e mora no adaptador, não aqui.

Para MonoGame/Blossom o adaptador consome a mesma decisão no present real
(`present_policy.c` já fazia certo) e transforma touch/cursor pela inversa da
MESMA decisão — nunca por outra policy.

## Readback (FV3): ler o arquivo não aprova nada

Depois de a engine aplicar e depois do primeiro present, o adaptador **mede**
drawable, content rect, policy efetiva e `video_config_generation` e chama:

```c
nxcompat_video_readback rb = { NXCOMPAT_VIDEO_OWNER_API_VERSION, ... };
if (nxcompat_video_readback_check(&out, &rb, line, sizeof line) != 0)
        report_mismatch(line);   /* NX-VIDEO-READBACK/1 ... mismatch=<campo> */
```

Um readback vazio (`api_version = 0`, nada medido) é **mismatch** por
construção: nenhum caminho aprova "o arquivo foi lido".

## Fronteiras que continuam físicas

Este componente é geometria e precedência puras. Content rect medido na tela,
barras opacas separadas do conteúdo no frame proof, e o `override.cfg`
realmente consumido pela engine são provas de aparelho e pertencem ao piloto.
