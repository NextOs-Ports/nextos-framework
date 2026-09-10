# Changelog — apkcompat (canonical APK compatibility contract)

## 1.1.0 — 2026-08-27 (item 9: bans de identidade cosmética + papéis)

- Correção pré-release da própria 1.1.0/engine 1.2.20: os papéis deixaram de
  ser apenas schema. O motor aplica `core_required`, `optional` e
  `variant_required` sobre o conjunto lógico base+splits e autentica
  `patch_selector` pelo SHA-256 do payload interno. Match único escolhe perfil;
  bytes ausentes/desconhecidos seguem o fallback comum. A decisão integra
  fingerprint, checkpoint/env de hook e marker.
- Fecha os caminhos alternativos de identidade: `source_validate`/`output_validate`,
  validação final/checkpoint do container, `source.patterns` ou destino
  `{basename}` baseado em nome externo, membros Android de assinatura/
  certificado e fonte local de hook empacotada. NXA0055 cobre referência a
  membro de assinatura em lógica customizada.
- Container `validate` também recusa a identidade cosmética: assinatura/certificado
  (NXA0004), nome do arquivo (NXA0005) e versionCode exato (NXA0006). Vão para
  `reference_build` (documentação). Um build re-assinado/renomeado/com versionCode
  diferente, estruturalmente idêntico, continua compatível.
- `compatibility.required_members` aceita, além de caminhos simples (cada um
  `core_required` implícito), objetos `{member, role[, variant]}` com papéis
  `core_required`/`optional`/`variant_required`/`patch_selector`
  (NXA0026..NXA0029). Caminho de membro continua sendo estrutura, nunca hash.
- `scan_static_suspects` também sinaliza fingerprint SHA-1 (40-hex) de
  certificado de assinatura (NXA0054), sem duplicar o 40-hex dentro de um 64-hex.
- Fixtures compartilhadas cobrem os bans e papéis + positivo metamórfico
  (re-assinado/renomeado/bumpado por estrutura = ACEITO); testes runtime cobrem
  APK/APKM/APKS/XAPK e Terraria `.4/.49`. A fixture sintética explícita
  `offtheroad-arm64-2-roles.json` instala containers renomeado/reempacotado sem
  `AVConfig.json`, exige `libgame.so` e `libc++_shared.so` AArch64 mais
  `assets/data_001.xpk` e rejeita package/ABI/ELF/dados divergentes.

## 1.0.0 — 2026-08-26 (V3, APK-COMPAT-01)

- First release of the single shared implementation of the container
  compatibility rule (NXA0001..NXA0053), replacing the three divergent
  copies in NXExtract, NXGenerator and NXRelease.
- Container identity (sha256/crc32 in any quantity, exact size) banned as a
  compatibility predicate; `reference_build` documentation block,
  `compatibility` decision block and `patch_profiles` with mandatory
  fallback introduced; hook contract schema
  org.nextos.apk-compat.hook-contract/1; static suspect scan.
- Shared fixtures `fixtures/apk-compat-cases-v3.json` including the Terraria
  `.4`/`.49` regression.
