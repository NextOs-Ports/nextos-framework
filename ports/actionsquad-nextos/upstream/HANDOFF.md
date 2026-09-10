# Action Squad — handoff técnico da fase Mali-450

## Estado confirmado

- alvo desta fase: somente NextOS AArch64 / Mali-450;
- loader atual SHA-256:
  `eaef29548f7e4bb9311acbcfba61eae67cf48583c1389919f7580b001fee8687`;
- fluxo confirmado: título → menus → episódio → missão → diálogo “Hostage
  Rescue” → gameplay aberta → movimento → prompt de interação;
- fase carregada pela engine:
  `all/media/levels/missions/01_01_slow_starters.dkas`;
- HUD nativa completa confirmada na gameplay (vida, arma, munição, itens e
  objetivos);
- áudio real confirmado no título e na gameplay, sem o estalo periódico do
  resampler antigo;
- menus em aproximadamente 30 fps; gameplay estabilizada em aproximadamente
  18,5 fps;
- `emustation.service` deve permanecer mascarado/inativo nos testes.

Evidência visual local mais direta: `shots/game_audio2.png` (diálogo),
`shots/game_audio_open.png` (gameplay), `shots/game_audio_moved.png` (movimento e
prompt) e `shots/mirror_game.png` (EBO espelhado final).

## Contrato nativo preservado

1. carregar e realocar `libAndroidEntryPoint.so`;
2. executar `JNI_OnLoad` com VM/JNI compatíveis;
3. chamar `ANativeActivity_onCreate`;
4. deixar o `android_native_app_glue` da engine criar sua própria thread/looper;
5. entregar janela, input queue, resume/focus e demais callbacks na ordem
   Android normal;
6. encerrar pela sequência de pausa/save, sem `_exit` inventado durante o boot.

Não chamar entry point de gameplay diretamente e não adiantar estado EGL/GL.

## Causas fechadas

### Neve sem mundo

O mundo era renderizado corretamente no atlas do FBO 1. Os composites finais
usavam `glDrawElements(..., GL_UNSIGNED_INT, ...)`; o driver Mali-450 não expõe
`GL_OES_element_index_uint` e devolvia `GL_INVALID_ENUM`. A extensão anunciada
ao jogo agora é verdadeira. Quando a engine ainda produz EBO uint32, o wrapper
valida que todos os índices cabem em 16 bits, cria um espelho uint16 residente
na GPU e conserva offsets/bindings. Nenhum passe ou shader foi removido.

### Áudio que congelava o avanço

O OpenAL embutido cria deadlines absolutos a partir de `CLOCK_REALTIME`. A ponte
pthread usava condvars monotônicas; os waits de fração de milissegundo viravam
esperas de décadas. `pthread_bridge.c` agora usa o clock realtime padrão. O shim
OpenSL também dispara callback por buffer realmente concluído, com uma thread
dedicada de 4 ms e contagem por bytes. A pipeline passou de 11 mil buffers sem
parar.

### HUD ausente

O `jni_shim.c` herdado devolvia `0.0f` para o método Java `GetDpi()`. A engine
dividia a diagonal em pixels por zero, gravava `mobile_screen_size=+inf` e
criava a câmera da interface com dimensões na casa dos bilhões. O método agora
devolve 240 dpi, consistente com a `AConfiguration` do runtime. Medido no
processo: `mobile_screen_size=6,1191864`; a HUD original voltou inteira, sem
overlay recriado pelo port.

### Áudio estalando/“estourando”

O OpenAL produzia blocos em 48 kHz enquanto a saída SDL operava em 44,1 kHz. O
resampler herdado reiniciava a fase e descartava amostras em toda callback,
criando saltos de até aproximadamente 31 mil entre blocos. `alsoft.conf` agora
solicita 44,1 kHz, igual ao runtime e ao device SDL. Uma captura do monitor da
saída em gameplay mediu pico de -12,35 dB, zero clipping e nenhum salto acima
de 8 mil.

### Assets e Bionic

`android_fopen` envolve `AAsset` em `funopen`, ausente na glibc. A ponte para
`fopencookie` mantém read/seek/close e evita o falso “asset ausente”. Também são
traduzidos os IDs Bionic de `sysconf`, `dlerror` consome o erro como POSIX e o
probe absoluto de OpenSL recebe a biblioteca virtual do shim.

## Defaults importantes

- não anunciar `GL_OES_element_index_uint` no Mali-450;
- correção global de NPOT/wrap desligada;
- conversão `GL_EQUAL`→`GL_LEQUAL` desligada, disponível apenas com
  `AS_DEPTH_EQUAL_LEQUAL=1` para diagnóstico;
- estatística GL desligada, disponível com `AS_GL_STATS=1`;
- injeção de bancada desligada, disponível somente com `COI_DEBUG_INJECT=1`;
- áudio ligado; `AS_NO_OPENSLES=1` serve somente para A/B de diagnóstico;
- OpenAL e SDL trabalham ambos em 44,1 kHz; não reintroduzir 48 kHz sem um
  resampler contínuo com fase preservada;
- `AS_MAX_SECONDS` serve somente para impor teto em teste automatizado.

## Limites e próxima fase

O build atual é específico do sysroot NextOS e exige até GLIBC 2.38. Não é
release pública, não é universal e não deve ser enviado ao R2. A fase universal
só pode começar por pedido explícito depois que este baseline Mali estiver
aceito; ela exigirá nome público canônico, framework versionado por opt-in,
GLIBC no máximo 2.30, instalador BYO-data e matriz física por família.
