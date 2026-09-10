# LANGUAGE-V2 — contrato do resolvedor de idioma (V3-SETTINGS-01)

API aditiva (`nxcompat_language_v2.h` / `nxcompat_settings.h`,
`NXCOMPAT_LANGUAGE_V2_API_VERSION 1`). A API legada
`nxcompat_language_select()` continua intocada, com a sua política própria
(env `NXPORT_LANGUAGE`/`GAME_LANGUAGE`, regra #5 anti-japonês). O V2 é um
resolvedor **puro e passivo**: entradas explícitas, snapshot imutável na
saída, zero efeito colateral.

## Ordem de resolução

```
1. session override        (pedido validado da sessão/launcher)
      | vazio/C/POSIX/inválido/sem match compatível
      v
2. settings file           (chave "language" do NEXTOSSETTINGS.txt,
      |                     via nxcompat_settings_parse)
      v
3. SDL / firmware          (preferência de locale do firmware/SDL)
      |
      v
4. POSIX locale            (LANG/LC_ALL — SOMENTE LEITURA)
      |
      v
5. fallback declarado      (OBRIGATORIAMENTE membro de `supported`;
                            senão, falha fechada — retorno != 0)
```

Cada degrau só decide se produz um tag que casa com a lista `supported`
do adapter (exato > língua+script > língua+região > língua). `C`,
`POSIX`, vazio, `auto` e valor inválido significam "sem preferência"
naquele degrau.

## Regra de não-mutação global

O framework NUNCA toca `LANG`, `LC_ALL` ou qualquer estado de locale do
launcher, do NXExtract ou do firmware: nada de `setenv()`, nada de
`setlocale()`. O locale POSIX entra como *string copiada pelo caller* e é
apenas lido. Quem precisa apresentar o idioma para a engine faz isso no
seu próprio processo, pelo sink registrado pelo adapter.

## Regra de segurança de script

Um pedido com script (explícito, ou implicado no chinês: CN/SG ⇒ Hans,
TW/HK/MO ⇒ Hant) **nunca** casa com um tag suportado de script diferente
— nem pelo casamento de língua nua. `zh-Hant` diante de uma lista só com
`zh-CN`/`zh-Hans` NÃO casa: a resolução simplesmente continua descendo a
ordem. Melhor inglês legível do que han simplificado no lugar do
tradicional.

## Modos de `language_access` (declarados no contrato do ADAPTER)

| modo               | significado                                            |
|--------------------|--------------------------------------------------------|
| `native-menu`      | o jogo tem menu de idioma próprio; o snapshot só sugere o valor inicial |
| `first-run-native` | o jogo pergunta o idioma na primeira execução; o snapshot pré-responde |
| `adapter`          | o adapter injeta o idioma no sink da engine (caso comum) |
| `single-language`  | o port só existe em um idioma; resolução é informativa |
| `none`             | idioma não é aplicável/controlável                     |

O modo é declarado pelo adapter no seu contrato (junto das capacidades do
port), nunca pelo framework.

## Sinks são do adapter

Android `Locale`, `Unity PlayerPrefs`/`Application.systemLanguage`,
Cocos, GameMaker, seleção de asset (`.lproj`, bundle por idioma, fonte):
**todos são registrados pelo ADAPTER**, no ponto correto do ciclo de vida
dele (antes do `JNI_OnLoad`, antes do primeiro `Resources.Load`, etc.). O
framework só fornece este snapshot passivo — ele não sabe nem quer saber
onde o valor é aplicado.

## Regra one-shot `post-settings-load`

Jogo que carrega as preferências DEPOIS do boot (lê PlayerPrefs/save no
meio da inicialização e sobrescreve o idioma): o adapter aplica o snapshot
uma única vez no gancho `post-settings-load`, depois que o jogo terminou
de carregar as preferências — e nunca mais. Reaplicar a cada frame ou a
cada resume briga com o menu nativo do jogo e viola o modo
`native-menu`.

## NEXTOSSETTINGS.txt

Parser estrito em `nxcompat_settings.h`: magic `# NEXTOS_SETTINGS/1` na
primeira linha não em branco, `chave=valor`, valores
`[A-Za-z0-9._-]{1,32}`, `quality` ∈ {auto, low, medium, high}, máx. 4096
bytes, UTF-8 estrito, sem NUL. Chave desconhecida é reportada (callback de
diagnóstico) e **rejeitada fail-closed** (V3-SETTINGS-01); só `language` e
`quality` são aceitas; chave conhecida duplicada é erro; qualquer
erro fatal devolve os defaults seguros (`language=auto`,
`quality=auto`). O parser recebe um buffer do caller — nunca abre
arquivo, nunca avalia nada.
