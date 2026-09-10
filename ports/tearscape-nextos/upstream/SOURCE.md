# Tearscape 0.2.19 — source snapshot / fonte

This repository publishes the port sources from
[`eefd466e1ac5f8b1bdff2c0f0b09c781946a8d14`](https://github.com/NextOs-Ports/nextos_ports_android/tree/eefd466e1ac5f8b1bdff2c0f0b09c781946a8d14/ports/tearscape),
under `ports/tearscape`. The copied files retain their exact contents and
executable modes. `SOURCE-PIN.json` lists their Git blob identities, sizes and
SHA-256 hashes. Existing license and credit files are preserved.

The build and packaging recipes retain their original repository context:
reproduction uses that immutable monorepo commit and its pinned framework
inputs. The standalone snapshot does not remove those dependencies.
See `build_low_glibc.sh`, `recipes/build_public_byo.py`, `FRAMEWORK-PIN.json`
and `PROJECT-BUILD-PIN.json` for the recorded inputs and recipe.

Release 0.2.19 preserves the existing `tearscape-nextos` engine with SHA-256
`3b5bceb99f03602574cd135cc82ebe03b87878a17291960591cf0d9fc0a5c00f`;
the engine was not rebuilt for this release. Validation of the 0.2.19 change
is limited to the host; no new physical device validation is claimed.
Game data is supplied by the owner and is not part of this source snapshot.

## Português

Esta árvore publica as fontes de `ports/tearscape` do commit imutável acima,
preservando conteúdo, modos executáveis, licenças e créditos.
`SOURCE-PIN.json` identifica cada arquivo por blob Git, tamanho e SHA-256.

As receitas mantêm o contexto original do monorepo e os inputs de framework
fixados. Para reproduzir, use esse commit e suas receitas; a cópia standalone
continua dependendo deles. A versão 0.2.19 preserva o executável e o SHA-256
indicados, sem recompilar a engine. A validação desta alteração é limitada ao
host, sem nova prova física em aparelho. Os dados do jogo são fornecidos pelo
dono e não acompanham esta árvore de fontes.
