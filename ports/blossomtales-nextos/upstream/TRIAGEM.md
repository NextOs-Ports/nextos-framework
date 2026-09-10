# Blossom Tales: The Sleeping King — TRIAGEM (27/08/2026)

**Veredito: 🟢 VERDE — GO.** Mono-Android/MonoGame, a família que este repositório já
domina (`stardewvalley`, `tmntsr`, `scourgebringer`). Perfil mais FÁCIL que os três.

> ⚠️ Correção: a primeira passada deste arquivo reprovou o jogo por "precisa de ART/dex"
> e "não tem GL nativo". Ambas erradas. O so-loader **não roda dex**: ele sobe o MonoVM
> direto, registra os métodos nativos do ACW na mão a partir da lista do dex, e o
> `sdv_egl_bridge` entrega ao `MonoGameAndroidGameView` um contexto **GLES2 criado pelo
> SDL**. Isso é exatamente o que os três ports Mono já fazem em produção.

## Pacote
- Origem: extraído com SAI no moto g(100) (`/sdcard/Download/`)
- Arquivo: `byo/com.FDGEntertainment.BlossomTales.gp-1.0.0-25.zip` (99.073.099 B)
- sha256: `2c9c6756aea948a3aef2313082d20b1c584c85883551bd74cae996140ebf32aa`
- Pacote: `com.FDGEntertainment.BlossomTales.gp` — versionName 1.0.0, versionCode 25
- Splits: `base.apk` (96,9 MB) + `arm64_v8a` (22,2 MB) + `es` + `pt` + `xxhdpi`
- ABI: **só arm64-v8a** — caso BOM (ARM64 é o padrão)
- **Sem PairIP** (`libpairipcore.so` ausente). Sem muro.

## Engine
MonoGame sobre .NET for Android (MonoVM), **AOT**:
- `libmonodroid.so`, `libmonosgen-2.0.so`, `libxamarin-app.so`, `libassembly-store.so`
- Jogo: `libaot-BlossomTalesAndroid.dll.so` (7,3 MB) + `libaot-MonoGame.Framework.dll.so`
- Áudio: `libopenal.so` embutido (OpenAL Soft), backend OpenSL ES
- Assets: **1030 `.xnb`** (content pipeline XNA, como Stardew) + **347 `.tmx`** (Tiled)
  + 43 `.m4a`, soltos e abertos em `assets/Content/` e `assets/Maps/`. 173,6 MB descompactado.

## Por que é o caso mais fácil dos quatro

| Ponto | Blossom Tales | Precedente |
|---|---|---|
| ACW do MonoGame | `crc64493ac3851fab1842` — `AndroidGameActivity`, `MonoGameAndroidGameView`, `OrientationListener`, `ScreenReceiver` | **CRC IDÊNTICO** nos três ports. O shim JNI do MonoGame casa direto, sem adaptação |
| Assembly store | em `lib/arm64-v8a/libassembly-store.so` — o caminho normal | scourgebringer teve o caso DIFÍCIL (store dentro do APK, `runtimeApks` reduzido). Aqui não precisa |
| Áudio | OpenAL Soft embutido | igual `tmntsr` (OpenAL → socket PulseAudio). Muito mais simples que o FMOD Studio do scourgebringer |
| Textura | pixel art 2D, tudo `SurfaceFormat.Color` | mesma decisão do scourgebringer: política de textura **desligada** por padrão |
| Tamanho/RAM | jogo 2D pequeno, 173 MB de asset | scourgebringer roda em ~117 MiB RSS sem swap. Este é menor |
| ABI | só arm64 | padrão do repositório |

## Trabalho previsto (tudo com peça pronta na árvore)
1. Reusar o loader Mono inteiro de `ports/scourgebringer` (`so_util`, `jni_shim`,
   `bionic_shims`, `sdv_egl_bridge`, `mono_trace`) — é o mais recente dos três.
2. Registrar os ACW específicos deste jogo: `crc64fb467d66f1819016`
   (`BillingManager`, `PlayGamesManager_*`, `ReviewManager_*`) e `crc64f0146600faa7a777`
   (internals do BillingClient). **Usar a lista de métodos nativos do dex** — a lição do
   scourgebringer: entrada a mais faz a registração inteira falhar.
3. **Play Games sign-in**: há `PlayGamesManager_AuthListener`. Esperar o mesmo travamento
   de título do scourgebringer; a solução já existe — entregar
   `onActivityResult(RESULT_CANCELED)` na thread do jogo para cair no branch offline.
4. Billing/Review: neutralizar offline (o port é local).
5. `alsoft.conf` da CFW muta OpenAL **embutido** → aplicar a capability
   `audio.embedded-openal` (lição já registrada).
6. Saída por `SELECT + START`; `_exit(0)` no teardown.

## Riscos
- Baixos. O único desconhecido real é o carregamento de `.tmx` (Tiled) em runtime —
  mas é I/O de arquivo comum, resolvido pelos shims de asset já existentes.
