# Sally Face 1.5.51 — recon medida (Mali-450)

> Medições feitas em 25/08/2026 sobre o container original. **Não remedir**;
> completar apenas o que estiver marcado como aberto.

## Identidade

| campo | valor |
|---|---|
| package | `com.portablemoose.sallyface` |
| versionName / versionCode | `1.5.51` / `50` |
| engine | Unity **2022.3.62f3** IL2CPP |
| metadata | v31 **em claro** (`AF1BB1FA`, 5,6 MB) |
| ABI | **arm64-v8a apenas** — o caso bom da regra #12b |
| activity | `com.unity3d.player.UnityPlayerActivity` (padrão, não custom) |
| orientação | `screenOrientation="0"` = landscape |
| container | APK único e completo, 273 arquivos, **sem OBB** |
| asset pack | `com.android.dynamic.apk.fused.modules = UnityDataAssetPack,base` → **fundido na base** |

## Objetos nativos

O port carrega **só três**:

```
libmain.so     6.728 B     loader Unity de fábrica (só carrega libunity + libil2cpp)
libunity.so   18.152.568 B
libil2cpp.so  31.534.712 B
```

🧹 **Nunca carregados** (camada de repack do container):

```
libhook.so       1.260.104 B   usa DobbyCodePatch (hook inline de codigo)
libstub.so         988.920 B   exporta só JNI_OnLoad, strings hex-ofuscadas,
                               referencia android/content/pm/Signature
```

O `classes.dex` chama `System.loadLibrary` para as duas; o port
não carrega nenhuma das duas e nunca executa o `classes.dex`.

## Pipeline e render — tudo favorável

- `GraphicsSettings.m_CustomRenderPipeline = 0` e **zero símbolo de URP/HDRP** no
  metadata → pipeline **BUILT-IN**. Não existe frente de URP neste port.
- `PlayerSettings.m_ActiveColorSpace = 0` = **GAMMA** → zero patch de color space.
- `boot.config` de fábrica:
  ```
  gfx-disable-mt-rendering=1
  wait-for-native-debugger=0
  hdr-display-enabled=0
  androidStartInFullscreen=1
  androidRenderOutsideSafeArea=1
  ```
  O render já é single-thread **de fábrica**.
- Tier1: Forward, `useHDR: false`, sem cascaded shadows.

### Contrato de display no host universal

No facade SDL, o drawable era medido corretamente, mas não era propagado ao
`ANativeWindow` falso, que permanecia em 1280×720 e ainda podia ser sobrescrito
por uma leitura tardia de `/dev/fb0`. Os canais JNI de métricas também não
existiam. Em uma saída 640×480 isso produzia três fontes de geometria
divergentes antes de a Unity criar o backbuffer.

A correção Via A publica o drawable medido, antes de `initJni`, em EGL,
`ANativeWindow`, `DisplayMetrics` e `Display`. `fill` não altera estado GL; em
1280×720 é identidade. `native` preserva o contrato lógico antigo como rollback.
Essa conclusão tem prova estrutural e golden host, mas ainda não prova os pixels
finais em hardware.

## Áudio

FMOD **interno da Unity** (`fmod_output_opensl.cpp` / `fmod_output_audiotrack.cpp`
dentro do `libunity.so`, classe java `org/fmod`). **Não é FMOD Studio** e não há
`libfmod.so` no container — o caminho `org/fmod/AudioDevice` → SDL do adapter já cobre.

Volume: **119,1 MB** — 505 Vorbis + 36 ADPCM.

## Perfil do conteúdo

2D puro, aventura em 5 episódios, **227 cenas** (`portableMooseSplash`, `mainMenu`,
`Episode 1..5/lvlNN`).

```
81.067 MonoBehaviour · 36.783 GameObject · 36.777 Transform · 20.234 SpriteRenderer
 3.982 Sprite ·  192 SpriteAtlas · 2.767 BoxCollider2D · 2.558 AudioSource
 1.447 AnimationClip · 992 Texture2D · 990 Camera · 955 Animator · 541 AudioClip
   284 MeshRenderer
```

Framework de jogo: **Adventure Creator** (`Assets/AdventureCreator/...`) sobre
scripts próprios em `Assets/Scripts/TKoU/...`. **Sem Rewired, sem InControl e sem
o novo Input System** — o jogo usa `UnityEngine.InputLegacyModule`.

## 🟡 DRM — PairIP na variante LEVE, stubável

- `com.pairip.licensecheck.*` está no `classes.dex` (`LicenseActivity`,
  `LicenseClient`, `ILicenseV2ResultListener`, `LicenseCheckException`,
  `com.pairip.application.Application`) + `uses-permission
  com.android.vending.CHECK_LICENSE`.
- 🎁 **Não existe `libpairipcore.so`** → é PairIP **só-license (Java)**, a mesma
  variante do Easy Delivery Co. A variante VM, que seria NO-GO, não está aqui.
- Também Java-side: **Google Play Games** (`gms.games` 2.1.0, APP_ID 109278133976)
  e **Play Asset Delivery** (`AssetPackExtractionService`, `getPackLocation`).
- 🚨 **PAD sem resposta definitiva faz `Resources.Load` devolver NULL e a cena
  nunca monta.** Todo stub responde OK/neutro e **definitivo**: nunca erro, nunca mudo.

## 🟠 Muro 1 — shaders só em GLES3, mas o engine aceita GLES2

63 shaders, **todos built-in, zero shader custom**. Subprogramas medidos:

| plataforma | subprogramas |
|---|---|
| GLES3 (4) | 492 |
| SPIR-V / Vulkan (25) | 486 |
| GLES3.1 (3) | 6 |
| GLES3.1-AEP (2) | 3 |
| **GLES2 (5)** | **0** |

🎁 O `libunity.so` é Unity 2022.3, que **ainda tem o backend GLES2 inteiro vivo** —
medido no binário: `force-gles20`, `force-gles30`, `force-gles31`, `force-gles32`,
`<OpenGL ES 2.0>` e mensagens `GLES20:`. **Não aparece** a frase
`OpenGL ES 2.0 is no longer supported`, que é o que mata o Unity 6.

Faltam só as **variantes ES2 nos dados**, e são poucas e triviais: `Sprites/Default`,
`Sprites/Mask`, `UI/Default`, `UI/Default Font`, `TextCore/Distance Field SSD`,
blits e GUI internos.

## 🔴 Muro 2 — textura e RAM. A conta está fechada.

Estado de fábrica (medido em `datapack.unity3d` 559 MB + `data.unity3d` 27,5 MB):

| formato | qtd | MB |
|---|---|---|
| `ETC2_RGBA8` | 503 | 812,7 |
| `RGBA32` (cru) | 311 | 441,1 |
| `RGB24` (cru) | 112 | 134,5 |
| `ETC2_RGB` | 76 | 47,8 |
| **total** | **1.002** | **1.436,1** |

Nada crunched, nada ASTC. Pior `sharedassets` isolado 84 MB; `resources.assets` 324 MB.

**A armadilha:** `ETC2_RGBA8` já é 8 bpp e ETC1+alpha também é 8 bpp (4+4), então os
503 arquivos que são 57% do acervo dão ganho **1,0×**. Trocar de formato é obrigatório
por **compatibilidade** (o Utgard não come ETC2), mas **não é o que salva a RAM**.

| estratégia | total | pior cena | maior textura |
|---|---|---|---|
| hoje | 1.436 MB | 84 MB + resources 324 MB | 6252 px ❌ |
| ETC1 dupla 1:1 | 993 MB (1,45×) | 31 MB + resources 204 MB | 6252 px ❌ |
| **ETC1 dupla + 2× seletivo (>1024 px)** ⭐ | **321 MB (4,5×)** | **8,2 MB + resources 79,5 MB** | **3126 px ✅** |
| ETC1 dupla + 2× geral | 248 MB (5,8×) | 7,7 MB + resources 51 MB | 3126 px ✅ |

**Política canônica atual:** restaurar as 1.002 texturas a partir do container
íntegro, manter ETC1 dupla camada somente como conversão de formato e não fazer
corte global. As únicas exceções são `os3_BG` e `os3_BG2`, reduzidas offline uma
vez porque ultrapassam o limite físico de 4096 px. A linha “ETC1 dupla 1:1” da
tabela é estimativa de acervo, não pico residente; o pico real precisa do gate
no Mali-450 de 1 GB. Qualquer redução adicional exige medição e allowlist por
asset, nunca um threshold genérico.

**Regressão fisicamente reproduzida:** a árvore antiga havia reduzido
`hospitalRoom_atlas` e o atlas global do Sal de 2048×2048 para 1024×1024 sem
reescrever os recortes de `Sprite`/`SpriteAtlas`. Por isso o menu (imagem inteira)
funcionava, mas a primeira sala aparecia em fragmentos e o personagem sumia. O
mesmo defeito ocorreu com ETC1 ligado e desligado, descartando encoder e shader;
restaurar os atlas corrigiu a sala escura, a sala acesa e o diálogo sem nenhuma
mudança no tradutor ES3→ES2.

🎁 **O jogo já tem a dupla camada no shader dele.** Keywords medidos:
`Sprites/Default` e `Sprites/Mask` têm `ETC1_EXTERNAL_ALPHA`. RGB em ETC1 + alpha em
ETC1 no `_AlphaTex` com esse keyword ligado é **caminho nativo do próprio shader**.
⚠️ `UI/Default` **não** tem esse keyword. O bridge injeta `_AlphaTex` quando o
fragmento amostra `_MainTex`; se um shader não puder consumir o segundo plano,
a textura usada por ele é reconstruída como RGBA8888. Não existe conversor 4444
neste adapter.

🔴 **Bloqueio duro que o 2× resolve de brinde** — duas texturas estouram o
`GL_MAX_TEXTURE_SIZE` do Mali-450 (4096):

```
os3_BG   6252 x 1607   sharedassets106.assets
os3_BG2  6018 x  833   sharedassets106.assets
```

Sem tratar, essas cenas carregam em branco. Com o 2× viram 3126 e 3009 px e passam.

## Referências usadas

`ports/blasphemous` (R2 — GLES3→GLES2 2D, `essl1.c` + `shader_gles2_patch.py`;
mesmo trio `libmain`/`libunity`/`libil2cpp`, Unity 2022.3.39f1 = mesma minor),
`ports/cuphead/src/etc1.[ch]` (dupla camada), `ports/strangerthings`
(`etc2_decode.c`, `texture_convert.c`).
