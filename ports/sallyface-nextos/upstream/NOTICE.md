# Notice / Aviso

## Runtime versions / Versões do runtime

This release was generated from one immutable runtime closure. The versions
below are the ones the package declares and audits:

Esta release foi gerada a partir de uma única closure imutável de runtime. As
versões abaixo são as que o pacote declara e audita:

| Component / Componente | Version / Versão |
|---|---|
| framework | `v3` |
| nxbootstrap | `0.6.37` |
| nxgenerator | `0.2.20` |
| nxrelease | `0.2.43` |
| NXExtract | `1.2.21` |
| NXSplash | `0.1.2` |
| nxinput | `0.5.1` |
| nxcompat | `0.3.0` |
| nxgl | `0.2.17` |

`GENERATION.json` binds the generator/template/runtime bytes of the published
package. The runtime modules actually compiled into the loader — `nxinput`,
`nxcompat` and `nxgl` — are vendored under `vendor/` in this repository, at the
same versions listed above, so the published binary is reproducible from this
source alone.

`GENERATION.json` amarra os bytes de gerador/template/runtime do pacote
publicado. Os módulos de runtime realmente compilados no loader — `nxinput`,
`nxcompat` e `nxgl` — estão vendorizados em `vendor/` neste repositório, nas
mesmas versões listadas acima, de modo que o binário publicado é reproduzível
apenas com este código.

The published ZIP also carries `nxsplash-nextos` 0.1.2, the MIT-licensed
pre-runtime splash screen of the NextOS framework, as a prebuilt binary; its
source is not part of this repository. The loader itself, `sallyface-nextos`,
is GPL-3.0-only and is fully reproducible from this source with
`build-universal.sh`.

O ZIP publicado também leva o `nxsplash-nextos` 0.1.2, a tela de espera do
framework NextOS licenciada sob MIT, como binário pré-compilado; o código dele
não faz parte deste repositório. O loader em si, `sallyface-nextos`, é
GPL-3.0-only e é integralmente reproduzível a partir deste código com o
`build-universal.sh`.

## Game data / Dados do jogo

Sally Face is © Portable Moose. This is an independent interoperability
project, with no affiliation with or endorsement from Portable Moose or Unity
Technologies. No game binary, asset, bundle, APK or audio track is distributed
here: the package is BYO-data and reads the copy the owner provides. The
screenshots in `docs/images/` show the port running and are used for
identification and documentation only.

Sally Face é © Portable Moose. Este é um projeto independente de
interoperabilidade, sem afiliação ou endosso da Portable Moose ou da Unity
Technologies. Nenhum binário, asset, bundle, APK ou faixa de áudio do jogo é
distribuído aqui: o pacote é BYO-data e lê a cópia fornecida pelo dono. As
capturas em `docs/images/` mostram o port em execução e servem apenas para
identificação e documentação.
