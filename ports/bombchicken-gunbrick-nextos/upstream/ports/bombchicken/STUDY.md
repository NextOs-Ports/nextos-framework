# Bomb Chicken → NextOS Mali-450 — STUDY (recon)

**Data:** 2026-08-07 · **Slug:** `bombchicken` · **Veredito: VIÁVEL — VERDE.**
Unity IL2CPP, **mas com variantes de shader GLES2 compiladas no build** — que é
exatamente o que faltava no Pikuniku. Perfil idêntico ao já mapeado em
`reference_unity2022_soloader_armadilhas`.

---

## 1. Fonte

- **APK de estudo:** cópia BYO local e não rastreada do Bomb Chicken v44/build 45
  (127,7 MiB); caminho pessoal deliberadamente omitido.
- v44 (build 45). **arm64-v8a apenas** — o device é aarch64, sem problema.
- `gamedata/game.apk` = APK inteiro staged. ⚠️ A Unity lê assets **de dentro de um ZIP**
  (`ApplicationInfo.sourceDir`), não de pasta — armadilha #6 da lição de Unity 2022.

## 2. Engine

| Item | Valor |
|---|---|
| Unity | **2022.3.39f1 LTS** (4 ocorrências na `libunity.so`; `data.unity3d` confirma) |
| Libs | `libil2cpp.so` **39,4 MiB** · `libunity.so` **16,8 MiB** · `libmain.so` 6,7 KiB |
| `libunity` NEEDED | `libmain libandroid liblog libz libEGL libmediandk libm libdl libc` |
| `libil2cpp` NEEDED | `liblog libm libdl libc` |
| UND | 287 símbolos |
| `il2cpp_*` exports | 239 |
| Proteção | **NENHUMA.** Sem PairIP, sem `libstub.so`, sem `libpairipcore.so` |

⚠️ **`libil2cpp.so` tem 4 `PT_LOAD`** → **mapear por IMAGEM**, não por "text + data"
(armadilha #8: o esquema antigo descarta o primeiro segmento e não carrega nada;
o `mmap` precisa de `PROT_EXEC`).

## 3. Gráficos — o dado que fecha o veredito

- **Unity 2022.3** ainda tem o backend GLES2 (a Unity só o removeu na **2023.1**).
  Strings presentes: **`force-gles20`** e `GLES20: vprog textures are used, but not supported.`
- **`GfxDeviceGLES` presente · `GfxDeviceVulkan` AUSENTE** — este build é GLES-only.
- 🟢 **E o build compilou os shaders de GLES2:** dos **72** objetos `Shader`,
  **67 têm variante `#version 100`** (GLSL ES 1.00). Também há 67 com `#version 300 es`
  e 2 com `310 es`, mas o caminho ES2 **existe nos dados**, não só na engine.
  *(Foi exatamente isso que faltou no Pikuniku, onde a `libunity` tinha 0 ocorrências
  de GLES2 e nenhum shader `#version 100`.)*
- `boot.config`: `gfx-disable-mt-rendering=1`, `androidStartInFullscreen=1`.
  ⚠️ **`m_MTRendering` serializado vence o `boot.config`** (armadilha #7) — se precisar
  forçar, é **patch de bytes** no `globalgamemanagers`; reescrever com UnityPy **perde
  objetos** que ele não reserializa.

### Texturas — praticamente sem trabalho

3528 `Texture2D` em `data.unity3d`:

| Formato | Qtd | RGBA32 | Mali-450 |
|---|---|---|---|
| RGBA32 | 3050 | 71,7 MiB | ok |
| RGB24 | 460 | 1,6 MiB | ok |
| Alpha8 | 8 | 88,0 MiB (nativo 22 MiB) | ok |
| **ETC2_RGBA8** | **10** | 9,0 MiB | **não suporta** |

**10 incompatíveis de 3528 = 0,3%.** Nativo o conjunto todo dá **~97 MiB** — cabe folgado
em 916 MiB. Os 10 ETC2 (maior: 2048×1024) resolvem com hook em `glCompressedTexImage2D`
decodificando para RGBA8888 (custo total ~9 MiB). Maiores texturas são 5× `Alpha8`
2048×2048 = 4 MiB cada nativos.

**Sem ASTC. Sem crunch de ETC2. Sem PVRTC.**

## 4. Áudio

`libunity.so` traz **FMOD** (o built-in da Unity) saindo por **OpenSL ES**
(`libOpenSLES` nas strings). `libmediandk` no NEEDED = decoder de mídia do Android para
áudio comprimido → provavelmente precisa de shim ou de converter/servir PCM.
`opensles_shim` do `kit_essencial` cobre o grosso.

## 5. Input — ⚠️ **Rewired**

O `global-metadata.dat` contém **`Rewired`** (e `InControl`, `GameController`, `Joystick`).
Rewired no Android lê o pad pela **ponte `InputDevice` do Java** — receita já paga e
documentada em **`reference_rewired_android_ponte_inputdevice_completa`**. É o mesmo
caminho do `ports/amongus`.

Não é jogo touch-first: **não precisa cursor nem toque sintético.**

## 6. Símbolos

287 UND. O scaffold classificou 150 automático (106 libc + 40 pthread + 4 cxx/log) e
137 "UNKNOWN" — mas **0 GLES / 0 EGL / 0 android** foram detectados porque a Unity
resolve GL e `libandroid` **em runtime por `dlopen`/`dlsym`**, não pelo NEEDED.

⚠️ **A Unity carrega o `libil2cpp` sozinha** (`ClassLoader.findLibrary` + `dlopen`) —
sem interceptar `dlopen`/`dlsym` ela sobe o diálogo *"Failed to load Il2CPP."* e **trava
esperando um botão que não existe** (armadilha #5).

## 7. As armadilhas de Unity 2022 que já pagamos

Lista completa em **`reference_unity2022_soloader_armadilhas`** (17 itens). As que mais
custam tempo, e que se aplicam aqui:

1. **`libunity` não exporta `Java_com_unity3d_player_UnityPlayer_*`** — registra ~44
   nativos via `RegisterNatives` dentro do `JNI_OnLoad`. Sem capturar esse array não
   existe `initJni` nem `nativeRender`. `initJni` é **static e recebe o Context**:
   `(JNIEnv*, jclass, jobject)`.
2. **O Choreographer é o muro** — objeto C++ com **herança múltipla**
   (`Handler.Callback` + `Choreographer.FrameCallback`). `nativeProxyInvoke` por nome
   devolve **null calado**. A chamada certa é o **slot da vtable secundária**
   (`+8` handleMessage, `+0x10` doFrame). Sem isso a main dorme para sempre em
   `pthread_cond_wait`.
3. **`sysconf`**: `_SC_NPROCESSORS_ONLN` é `0x61` no Bionic e `84` no glibc. Sintoma
   enganoso: 0 CPUs → `ArgumentOutOfRangeException: concurrencyLevel must be positive`.
4. **`__sF` é array de STRUCTS FILE** (`stdout` = `&__sF[1]`), não de ponteiros.
5. **`dlopen`/`dlsym` interceptados** (item 6 acima).
6. **Assets em ZIP stored**, apontado por `ApplicationInfo.sourceDir`.
7. **`m_MTRendering` por patch de bytes**, nunca reserializando com UnityPy.
8. **Mapear o `.so` por imagem** (4 `PT_LOAD`).
9. **jmethodID é por (nome, ASSINATURA)** e argumento vem em duas formas (`va_list` e
   array de `jvalue`).
10. **A Unity desreferencia o `jobject`** — objeto fake com primeiro campo inválido segfalta.
11. **`dl_iterate_phdr` próprio**, senão toda exceção C++ vira `Il2CppExceptionWrapper`.
12. **`sigaction` traduzido** (layout Bionic ≠ glibc) ou o SIGPWR do GC do IL2CPP mata o
    processo — o log só diz *"Falha de energia"*.
13. **`ReflectionHelper` não é opcional** — todo `AndroidJavaObject.Call(...)` passa por
    ele; o resultado vira jmethodID pelo **slot 7** do JNIEnv (`FromReflectedMethod`).
14. Ao desviar símbolo do `libil2cpp`, escrever **4 bytes, nunca 16** — os exportados
    ficam colados e um hook grande destrói os vizinhos.

**Ferramenta que pagou o investimento no Shadow Fight 2:** EGL/GLES nulo (`nullgl.c`) +
`qemu-aarch64` para dar boot na engine inteira **sem o aparelho**. Não prova pixel, prova
todo o resto — útil enquanto o device não estiver definido.

## 8. Referências (só port que FUNCIONA — regra #16)

- **Estrutural, port PUBLICADO:** `ports/hitmango` (v1.2.0, Unity so-loader).
- **Lições de Unity 2022 e Rewired:** as memórias citadas. `ports/shadowfight2` e
  `ports/amongus` são Unity 2022.3 mas **não estão terminados** → valem como **lição**,
  nunca como código a copiar.

## 9. Riscos

| # | Risco | Prob. | Resposta |
|---|---|---|---|
| 1 | Choreographer / `RegisterNatives` do `JNI_OnLoad` | **Alta** | É o muro conhecido; a saída é o slot da vtable secundária |
| 2 | Rewired não enxergar o pad | Média | Ponte `InputDevice` — receita pronta |
| 3 | `libmediandk` para áudio comprimido | Média | Shim ou servir PCM |
| 4 | 10 texturas ETC2 | Baixa | Hook em `glCompressedTexImage2D` → RGBA (~9 MiB) |
| 5 | Shaders ES2 existirem mas a engine escolher ES3 | Baixa | `force-gles20` no boot.config / forçar o device level |

**Sem muro previsível além do Choreographer**, que já tem solução documentada.

## 10. Plano

1. Loader arm64: kit + `dlopen`/`dlsym` interceptados + mapear por imagem (4 `PT_LOAD`).
2. `libmain` → `libunity` → `libil2cpp`; capturar o array de `RegisterNatives` do `JNI_OnLoad`.
3. `initJni(JNIEnv*, jclass, jobject)` → `nativeRecreateGfxState` → `nativeResume` →
   handshake do Choreographer → `nativeRender`.
4. GLES2 com **resolução lida do framebuffer** (regra #25). Conferir que pegou os shaders
   `#version 100`.
5. Áudio (FMOD/OpenSL). 6. Input Rewired. 7. Gameplay. 8. `nx-verify` verde + launcher
   limpo padrão PortMaster.
