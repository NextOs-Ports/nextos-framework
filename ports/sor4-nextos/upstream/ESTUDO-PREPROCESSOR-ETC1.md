# Estudo: Pré-processador ETC1 (progressor) — SOR4

Objetivo (pedido do porter): um **progressor** estilo Dysmantle (janela com % real + nome
dos arquivos) que, na **1ª execução**, converte TODAS as texturas pra **ETC1**, **apaga os
originais** (fica só ETC1), define o **texscale** e aplica outros fixes (fontes etc). Depois
o jogo abre rápido desde a 1ª vez, **zero conversão em runtime**. O Mali-450 lê ETC1 nativo.

## Estado atual (já feito)
- Encoder ETC1 pronto: `sor4_etc1_encode` no `libsor4astc.so` (do Dysmantle, correto) + decoder ASTC (`sor4_astc_decode`). Buildados arm64.
- Reader (`Texture2DReader.cs`) já sabe: ASTC opaca → ETC1 nativo / com alpha → RGBA8, gateado por `SOR4_ETC1=1`. Hoje converte EM RUNTIME e cacheia em `texcache` (lento na 1ª vez). Commit b94c411.
- ETC1 validado no device: sobe sem crash, `fmt=RgbEtc1 internal=Etc1 glFmt=CompressedTextureFormats`, RSS 376→286MB.

## 🎉 ACHADO DECISIVO (2026-06-17): texturas são XNB PADRÃO, não IAP!
Contagem em gameassets: **XNB=25224, IAP=19, outro=49**. Ou seja, as texturas (cenário
`decors/`, sprites `animatedsprites/`, `gui/`) são **XNB padrão** (magic `XNBa` = 'XNB'+target
'a'+version 5 + flags 0x40=**LZ4-comprimido**). Os 19 `\x00IAP` são minoria (dados especiais,
NÃO as texturas) → **NÃO são o bloqueio**. O conversor offline é totalmente viável com parser XNB.

### Spec do XNB (de ContentManager.GetContentReaderFromXnb):
- Header: 'X''N''B' + platform(1 byte, 'a') + version(1, =5) + flags(1) + xnbLength(int32 LE).
  flags: 0x80=LZX, 0x40=LZ4 (estes têm 0x40).
- Se comprimido: decompressedSize(int32) + dados. LZ4 = classe **`Lz4DecoderStream`** do MonoGame
  (build/MonoGame/.../Content/, REUSAR no conversor). LZX = LzxDecoderStream.
- Corpo descomprimido (ContentReader): typeReaderCount(7-bit int) + N×(nome string 7-bit-len + version int32)
  + sharedResCount(7-bit int) + objeto: typeReaderIndex(7-bit, 1-based) + dados do Texture2DReader.
- Texture2DReader: surfaceFormat(int32) + width(int32) + height(int32) + levelCount(int32) +
  por nível: size(int32) + dados (ASTC; bloco detectado por len/16 vs ceil(w/bx)*ceil(h/by)).

### Design do conversor (reusa tudo):
- .NET console tool (roda no device arm64 OU no PC) referenciando MonoGame.Framework.dll (p/ Lz4DecoderStream)
  + P/Invoke sor4astc (sor4_astc_decode + sor4_etc1_encode, já existem arm64).
- Pra cada gameassets/**/*.xnb: parse header → descomprime (Lz4DecoderStream) → lê manifest → se o
  typeReader for Texture2DReader: lê fmt/w/h/level0 ASTC → decode ASTC → (downscale por TEXSCALE) →
  alpha analysis → ETC1(opaca)/RGBA8(alpha) → **escreve XNB NOVO** (flags=0 NÃO-comprimido, mais simples;
  1 typeReader Texture2DReader, 0 shared, fmt=RgbEtc1/Color, w/h/(1 nível)/dados) → **sobrescreve o arquivo**.
- NÃO precisa GL (puro transform de dados). NÃO precisa reverter IAP (só 19 arquivos, pular/depois).
- Reader em runtime: lê RgbEtc1/Color direto (caminho ASTC nem dispara) → zero conversão no jogo.
- Multi-thread (4 cores) p/ acelerar os 25224. TEXSCALE aplicado AQUI (no pré-processo), correto.

## (obsoleto p/ texturas) O formato `\x00IAP` — só os 19 arquivos especiais
- Os assets de textura em `gameassets/` (nomes ofuscados 16-char) começam com `00 49 41 50 02 00 00 00 08 06...` = `\x00IAP` v2 + dados = **container OBFUSCADO**, não XNB puro.
- `CommonLib.AssetManagerWrapper.OpenInMemoryStream/ReadAllBytes` (IL) só leem o arquivo CRU (CopyTo MemoryStream) — **não decriptam ali**.
- A de-obfuscação + decode de textura é custom: `CommonLib.asset_cache/TextureStream`,
  `asset_cache.get_asset_stream` → OpenInMemoryStream → depois passa por um caminho que
  vira XNB (logs `[MG] ContentManager.GetContentReaderFromXnb`). FALTA localizar EXATAMENTE
  onde `\x00IAP` vira XNB (provável: ContentReader custom ou patch na ContentManager do host).
- **Sem reverter o `\x00IAP` não dá pra converter offline.** Próximo passo do estudo:
  achar a de-obfuscação (grep IL por TextureStream/asset_cache full + Program.cs/ContentManager
  do host; testar se é XOR simples — ver se `\x00IAP` XOR chave vira 'XNB'/'\x00\x00\x00\x01').

## Abordagens (decidir ao implementar)
- **A) Reescrever os dados (o que o porter quer):** decriptar IAP → XNB → ASTC → decode →
  ETC1/RGBA8 → reescrever o XNB (SurfaceFormat=RgbEtc1) → **re-obfuscar IAP** (ou salvar plano
  se o loader aceitar XNB sem IAP) → sobrescrever o arquivo. Apaga ASTC, fica ETC1. Precisa
  reverter IAP nos DOIS sentidos (decode+encode) OU confirmar que o loader lê XNB plano.
- **B) Cache por NOME + apagar originais:** mudar a chave do texcache de hash-do-ASTC p/ nome
  do asset; pré-popular o cache ETC1; apagar os `\x00IAP`. Reader lê ETC1 do cache sem abrir o
  original. Precisa passar o nome do asset até o Texture2DReader (hoje ele só vê o stream).
- **C) Device-side no progressor:** um tool arm64 (reusa libsor4astc.so) roda no progressor,
  converte in-place com % UI. Mesma necessidade de reverter IAP.

## Progressor (reuso pronto)
- Binário `progressor` (335KB) + protocolo: `.src` bash com `pbar <0-100>`, `msgbox`, `confirm`,
  `settitle` (JSON base64 em stdout, lê reply em stdin). Vem no DYSMANTLE v3.zip (tools/).
- Modelo: launcher chama `progressor --log ... tools/sor4_pretex.src` na 1ª execução (gate por
  marcador tipo `.textures_etc1`), roda o conversor com progresso, depois o jogo abre.
- Incluir no `.src`: conversão ETC1 + set texscale + fix de fontes + (gptokeyb pros controles).

## Pacote final (ZIP PortMaster-style)
launcher .sh + sor4host + MonoGame.Framework.dll + SOR4.dll(patched) + libs (libsor4astc/
libWwise/libSharpFont...) + gameassets (ETC1 após converter) + tools/{progressor, sor4_pretex.src,
common.src} + gptokeyb + .gptk. Testar: abre, roda, controla, leve.

## TODO ordem
1. Reverter o formato `\x00IAP` (decode; e encode se abordagem A). ← MURO ATUAL
2. Conversor offline/device (ASTC→ETC1, escolha opaca/alpha, reescreve+apaga original).
3. `sor4_pretex.src` com pbar + texscale + fontes.
4. gptokeyb + montar ZIP + testar.
