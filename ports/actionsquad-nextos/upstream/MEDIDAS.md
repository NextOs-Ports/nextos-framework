# Door Kickers: Action Squad 1.2.4 — medidas reproduzíveis

## Identidade do jogo

| Campo | Valor medido |
|---|---|
| APK | 61.225.272 bytes |
| SHA-256 | `53763a026dfdf0bbd96f4dc1aef6168bebbee55e313a1fde7146abde3f1c3bc3` |
| Pacote | `com.khg.actionsquad` |
| Versão | 1.2.4 (`versionCode 10241`) |
| ABI | `arm64-v8a` |
| Engine | `libAndroidEntryPoint.so`, 4.654.112 bytes |
| SHA-256 da engine | `d97e40f99e788ea13097e54e4726d5becd3788e93cae90a698ddb5fbaa773f41` |
| Assets | 831 arquivos, 103.830.815 bytes em `assets/all` |
| API nativa | NativeActivity, com `android_native_app_glue` dentro da engine |

## Superfície medida

- `DT_NEEDED`: `libm`, `liblog`, `libandroid`, `libz`, `libGLESv2`, `libEGL`,
  `libdl` e `libc`;
- 354 símbolos indefinidos no ELF Android;
- exports principais: `JNI_OnLoad`, `ANativeActivity_onCreate`,
  `Java_*_SetHaveController` e `Java_*_LoadNativeCloudData`;
- GLES2; o driver físico não oferece `GL_OES_element_index_uint`;
- OpenAL-soft 1.12.854 embutido, usando a superfície OpenSL ES;
- assets lidos por `AAssetManager` e `funopen`; dados locais gravados no cwd em
  `gamedata/`.

## Provas de execução

| Prova | Resultado |
|---|---|
| JNI + NativeActivity | `JNI_OnLoad` e `ANativeActivity_onCreate` executados antes do loop |
| Lifecycle | janela, input queue, resume e focus entregues à glue da engine |
| Nível | `Level::SetLevelState - Started game!` |
| Arquivo de missão | `all/media/levels/missions/01_01_slow_starters.dkas` |
| Diálogo | “Hostage Rescue” visível antes da gameplay |
| Gameplay | mundo, personagens, iluminação, HUD, movimento e prompt de interação |
| Áudio | mais de 11.000 buffers avançando; título −32,9 dB médio/−18,6 dB pico; gameplay −34,3/−16,8 dB |
| Menu | 29,4–30,5 fps |
| Gameplay | aproximadamente 18,5 fps / 54 ms por frame depois de estabilizar |

## Causa da tela de neve

O dump do FBO mostrou o nível pronto no atlas. Os dois draws que compunham esse
atlas sobre o fundo eram enviados como `GL_UNSIGNED_INT`; ambos falhavam com
`GL_INVALID_ENUM`. O fundo de neve usa outro draw e por isso continuava visível.

A correção mantém uma cópia dos EBOs de elemento e, quando o driver não suporta
índices uint32, prova que cada índice cabe em `uint16_t`. O espelho uint16 é
enviado uma vez à GPU em `glBufferData`; `glDrawElements` troca apenas binding,
tipo e offset proporcional, depois restaura o EBO original. Se qualquer índice
exceder 65535, a ponte recusa a conversão em vez de truncar.

## Causa do travamento de áudio

Três call sites da engine/OpenAL montam `pthread_cond_timedwait` com tempo
absoluto realtime. Interpretar o mesmo número como monotônico agenda o wakeup
para décadas no futuro. Condvars com clock realtime restauraram os timeouts
originais. O callback OpenSL roda fora da thread de render e ocorre uma vez por
buffer concluído, inclusive no buffer inicial de um byte usado pelo OpenAL.

## Interpretação de desempenho

O espelho EBO remove a conversão e cópia por draw, mas não alterou materialmente
os 18,5 fps. O custo dominante é a carga real de fragmentos da composição e da
iluminação em duas passagens no Mali-450, não CPU, áudio nem upload de índices.
Qualquer modo rápido que remova efeitos precisa ser tratado como opção futura e
nunca como default silencioso.
