# Blossom Tales: The Sleeping King — estudo do pacote e do runtime

Alvo: **Mali-450 Utgard (Amlogic)**, NextOS-Retro-Elite 4.9-Nexus, glibc 2.44, 1836 MB de RAM.
Base de código: `ports/scourgebringer` copiado inteiro (regra zero do port).

## 1. Pacote

| Item | Medido |
|---|---|
| Pacote | `com.FDGEntertainment.BlossomTales.gp` 1.0.0 (25) |
| Origem | export SAI, `byo/…-1.0.0-25.zip`, 99.073.099 B, sha256 `2c9c6756…bf32aa` |
| Splits | `base.apk` + `arm64_v8a` + `es` + `pt` + `xxhdpi` |
| ABI | **só `arm64-v8a`** |
| Activity | `com.castlepixel.blossomtales.Activity1` (ACW), gerenciada `BlossomTalesAndroid.Activity1, BlossomTalesAndroid` |
| PairIP | ausente |
| Assets | 1030 `.xnb` + 356 `.tmx` + 43 `.m4a`, abertos em `assets/Content` e `assets/Maps` (1431 arquivos, 188 MB) |
| Libs nativas | `lib/arm64-v8a/`, **STORED** (`extractNativeLibs=false`), 21 MB |

## 2. Motor — e por que ele NÃO é o mesmo do scourgebringer

MonoGame sobre .NET for Android, mas **uma geração à frente** dos três Mono já portados:

| | scourgebringer / tmntsr | **blossomtales** |
|---|---|---|
| `libmonodroid.so` | .NET for Android 13.2.99 / Xamarin | **.NET for Android 28.2.13676358** |
| MonoVM | — | **10.0.7** (`libmonosgen-2.0.so`) |
| Formato das mensagens | `printf` (`%s`) | `std::format` (`{}`) |
| Registro dos ACW | `mono.android.Runtime.register()` | **marshal methods** |
| `Runtime_init` | 11 argumentos, `apiLevel` no 3º slot de pilha | **8 argumentos** |
| Assembly store | dentro do APK (caso difícil) | `lib/arm64-v8a/libassembly-store.so` |
| AOT | recusado, JIT | `mono_aot_mode_name = "normal"` → JIT é legítimo |

### 2.1 `Runtime_init` de 8 argumentos (derivado no disassembly)

O nome mangled de `Java_mono_android_Runtime_initInternal` dá a assinatura exata:

```
P7_JNIEnv P7_jclass P8_jstring P13_jobjectArray S8_ SA_ i P8_jobject SA_ h h
  = (JNIEnv*, jclass, jstring lang, jobjectArray runtimeApks,
     jstring runtimeNativeLibDir, jobjectArray appDirs, int localDateTimeOffset,
     jobject loader, jobjectArray assemblies, uchar isEmulator, uchar haveSplitApks)
```

O wrapper `Java_mono_android_Runtime_init` (@0x84de0) move `x6→x7`, zera
`localDateTimeOffset`, copia `stack[0]` e zera `isEmulator`/`haveSplitApks`.
Logo o wrapper recebe **8**:

```
(env, klass, lang, runtimeApks, nativeLibDir, appDirs, loader, assemblies)
```

**Não há `apiLevel`.** Chamar com a assinatura do scourgebringer passa lixo pela pilha.

### 2.2 Marshal methods — a chave deste port

`libxamarin-app.so` exporta **88 símbolos**, entre eles o corpo nativo de cada método
do ACW já com o nome JNI curto:

```
Java_com_castlepixel_blossomtales_Activity1_n_1onCreate__Landroid_os_Bundle_2
Java_crc64493ac3851fab1842_MonoGameAndroidGameView_n_1surfaceCreated
Java_mono_android_TypeManager_n_1activate
```

Consequência prática: **a armadilha "um método nativo a mais derruba o registro
inteiro" não existe aqui** — não há registro, só resolução de símbolo. A lista do dex
continua sendo a referência do que *existe* (a `Activity1` declara 7 métodos nativos e
**não** declara `onActivityResult` nem `onWindowFocusChanged`), mas errar a lista não
derruba mais nada: o símbolo simplesmente não é encontrado.

Sequência de boot reproduzida pelo loader:

```
JNI_OnLoad(vm) → Runtime_init(8) → TypeManager.n_activate("BlossomTalesAndroid.Activity1, …")
              → Activity1.n_onCreate → MonoGameAndroidGameView.n_surfaceCreated/Changed
              → Activity1.n_onResume → laço de input
```

## 3. Muros encontrados, na ordem

| # | Sintoma | Causa real | Correção |
|---|---|---|---|
| 1 | SIGSEGV logo depois de `debug.mono.log`, PC na libc | `ALooper_forThread/acquire/addFd/removeFd` são imports **novos** do `Runtime_initInternal`; sem shim o PLT saltava para lixo | stubs de `ALooper_*` em `imports.gen.c` |
| 2 | `InvalidCastException` em `HapticFeedback.Initialize` | `getSystemService` voltava `java.lang.Object` genérico | mapa serviço→classe; **e o Java.Interop chama esse método pela via NÃO-virtual**, então o ramo tem de estar em `CallNonvirtualObjectMethod*` |
| 3 | `NullReferenceException` em `BillingManager.Initialize` | `BillingClient.newBuilder(Context)` devolvia NULL | objeto tipado pela **assinatura** do método (`typed_object_from_signature`), aplicado nos defaults de instância e estáticos |
| 4 | Jogo mudo, sem erro de OpenAL (o device pulse abria e ficava `RUNNING`) | `OpenALSoundController` do MonoGame faz `int.Parse` no retorno de `AudioManager.getProperty`; a string genérica do shim estourava o parse e matava o áudio inteiro | `getProperty` devolve `48000` / `512` |

Nenhum muro de textura: é pixel art, tudo `SurfaceFormat.Color`, e a política herdada do
tmntsr fica **OFF** (como no scourgebringer).
Nenhum muro de `.tmx`: o carregamento em runtime passou pelos shims de asset existentes —
o save grava o nome do mapa (`Overworld-Bcastle-house1-LilysHouse.tmx`).

## 4. Google Play Games

O jogo tem `PlayGamesManager_AuthListener/_ShowUiListener/_UnlockListener`. **O título NÃO
travou** como no scourgebringer: a Task falsa do shim completa na hora
(`addOnCompleteListener` → `n_onComplete`) e o jogo cai sozinho no branch offline.
O `n_onComplete` de cada listener é resolvido **pela classe do próprio listener**
(`jni_marshal_for_object`) — são cinco classes com o mesmo nome de método, e um
`n_onComplete` genérico chamaria o listener errado.
O ramo de `onActivityResult` do scourgebringer foi preservado mas é **inerte** aqui: a
`Activity1` não declara esse método e o símbolo não existe no `libxamarin-app`.

## 5. Áudio

`libopenal.so` embutido é redirecionado, no `dlopen`, para o **OpenAL do sistema**
(1.25.2, backend PulseAudio) — mesmo caminho do tmntsr. Por isso a capability
`audio.embedded-openal` **não** se aplica: quem toca é o OpenAL do host, e o
`alsoft.conf` da CFW (ausente neste aparelho) agiria sobre um build compatível.

## 6. Assets e memória

- Política de textura **desligada** (pixel art). Não medir de novo antes de ter motivo.
- RSS medido em jogo: **~93–116 MiB**, `VmSwap=0`, ~1,3 GB ainda disponíveis.
- Quadro: 42–56 fps no menu/cutscene, 30–35 fps no overworld.
