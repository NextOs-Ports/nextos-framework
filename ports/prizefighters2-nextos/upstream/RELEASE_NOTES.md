# Prizefighters 2 v1.0.3 — public universal ARM64 release

This release fixes installation on the PortMaster family. On muOS the v1.0.2
package produced no visible port entry at all, and a hand-moved launcher
returned to the ports list without writing a single log line. Gameplay,
controls and data preparation are unchanged from v1.0.2.

## Fixes

- The ZIP now uses the canonical PortMaster layout shared by every other
  NextOS port: `Prizefighters 2.sh` and the `pf2/` folder at the root,
  extracted directly into `roms/ports/`. v1.0.2 wrapped the release in
  `ports/` + `ports_scripts/`, which muOS never scans.
- The entry script resolves its own real path and then searches beside itself,
  one and two levels up, and the known card roots: `/roms`, `/roms2`,
  `/storage/roms`, `/mnt/mmc`, `/mnt/sdcard`, `/mnt/union`, `/userdata`, with
  and without `roms`.
- No more silent failure. If the `pf2` folder or `bash` is missing, the
  launcher writes `pf2-launcher-error.log` next to the script and prints the
  reason on the console. Errors raised before the `pf2.log` redirect are
  reported the same way.
- Installation documents state the per-firmware layout, including the muOS
  `mmc/roms/ports` path.

It is a native, BYO-data port: the ZIP does not contain the game, an
APK/XAPK, a save, a receipt or purchase state.

## Installation

1. Extract `Prizefighters.2.NextOS-v1.0.3.zip` **into the `ports` folder** of
   the ROM card — `roms/ports/` on ArkOS/ROCKNIX/Knulli, `mmc/roms/ports` on
   muOS. `Prizefighters 2.sh` and the `pf2` folder must sit side by side.
   On NextOS/EmuELEC also copy the `.sh` to `roms/ports_scripts/`.
2. Put a legally owned Prizefighters 2 **v1.09.3 XAPK** containing
   `config.arm64_v8a.apk` in `ports/pf2/gamedata/`.
3. Start **Prizefighters 2** from Ports and let NXExtract finish once.
4. Use **Select + Start** to exit.

Premium migration is deliberately not fabricated. A legitimate Android owner
export can be imported for testing, but entitlement transfer remains
experimental and depends on what the original game accepts outside Google Play.

SHA-256:

```text
34d1e96bef39f6acb9cdcf314f3b9aeda09a8f1fb9fb87e7b318d2064fd4bbb2  Prizefighters.2.NextOS-v1.0.3.zip
```

---

# Prizefighters 2 v1.0.3 — release pública universal ARM64

Esta release conserta a instalação na família PortMaster. No muOS o pacote
v1.0.2 não mostrava nenhuma entrada do port, e mover o launcher na mão fazia
o jogo voltar para a lista de ports sem gravar uma linha de log. Gameplay,
controles e preparação de dados continuam iguais aos da v1.0.2.

## Correções

- O ZIP passa a usar o layout PortMaster canônico dos outros ports do NextOS:
  `Prizefighters 2.sh` e a pasta `pf2/` na raiz, extraídos direto em
  `roms/ports/`. A v1.0.2 embrulhava a release em `ports/` + `ports_scripts/`,
  e o muOS não varre `ports_scripts`.
- O launcher resolve o próprio caminho real e procura ao lado dele, um e dois
  níveis acima, e nas raízes conhecidas de cartão: `/roms`, `/roms2`,
  `/storage/roms`, `/mnt/mmc`, `/mnt/sdcard`, `/mnt/union`, `/userdata`, com e
  sem `roms`.
- Acabou a falha muda: sem a pasta `pf2` ou sem `bash`, o launcher grava
  `pf2-launcher-error.log` ao lado do script e escreve o motivo no console.

## Instalação

1. Extraia o `Prizefighters.2.NextOS-v1.0.3.zip` **dentro da pasta `ports`**
   do cartão — `roms/ports/` no ArkOS/ROCKNIX/Knulli, `mmc/roms/ports` no
   muOS. O `Prizefighters 2.sh` e a pasta `pf2` ficam lado a lado. No
   NextOS/EmuELEC, copie também o `.sh` para `roms/ports_scripts/`.
2. Coloque o XAPK legal **v1.09.3** com `config.arm64_v8a.apk` em
   `ports/pf2/gamedata/`.
3. Abra **Prizefighters 2** em Ports e deixe o NXExtract terminar uma vez.
4. **Select + Start** sai do jogo.

A importação de save legal cria backup e não carrega a resolução do celular.
Transferência de premium continua experimental; o port não inventa entitlement,
recibo nem contorna autenticação da loja.
