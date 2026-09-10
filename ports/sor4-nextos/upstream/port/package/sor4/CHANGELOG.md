# Changelog — Streets of Rage 4

## 2.0.2
- Os controles de música e efeitos agora alimentam também o mixer OpenAL que produz o
  áudio audível. Foi removido o piso que sobrescrevia `MusicVolume`/`SfxVolume` para 100%
  a cada quadro e gerava dezenas de milhares de escritas no `wwise.log`.
- O render target mínimo de reflexos passa a nascer transparente. Quando reflexos estavam
  desligados, o jogo ainda amostrava esse alvo 32×32 sem nunca limpá-lo; o conteúdo indefinido
  podia aparecer como poças brancas ou manchas de óleo dependendo do driver/GPU.
- Varreduras de pico/click do OpenSL ficam restritas a `SOR4_NATLOG=1`. O `wwise.log`
  começa limpo a cada execução e termina com um resumo compacto de efeitos, streams e
  músicas, para que um relatório corresponda a uma única sessão.
- O wrapper Wwise público é reconstruído para AArch64 no builder Debian Buster e o
  release bloqueia qualquer requisito de GLIBC posterior à 2.30.

## 2.0.1
- Compatibilidade segura com APKs v1.4.5 repacotados que preservam todos os dados
  oficiais, mas omitem exatamente uma textura XNB: 25.904 assets passam a ser aceitos
  sem afrouxar a validação do conjunto completo de 25.905.
- A exceção exige as bibliotecas/código oficiais, os 613 WEM, todos os arquivos não-XNB
  e seus tamanhos/CRCs exatos. Três acumuladores SHA-256 independentes provam que a árvore
  XNB é a oficial menos um único registro; APK alterado, áudio faltando ou arquivo
  não-textura ausente continua sendo recusado.
- Se o jogo pedir a XNB ausente, o bridge usa `blank.xnb` fornecido pelo próprio APK e
  registra `[asset COMPAT]` uma vez no log. O fallback só é ativado pelo marcador
  transacional de 25.904 assets; instalações completas continuam estritas.
- A publicação transacional agora percorre toda a allowlist antes de decidir cada caminho,
  evitando mensagens `Broken pipe` inofensivas durante o commit dos assets.

## 2.0
Passagem de compatibilidade: o alvo deixou de ser "o aparelho da bancada" e passou a ser
qualquer portátil AArch64 de 1 GB, seguindo o que já estava provado nos ports do Bully e
do Sonic 4 Episode II. Nenhum backend de vídeo ou de áudio é forçado em lugar nenhum.

- **Bibliotecas do firmware primeiro, pacote depois.** A busca de bibliotecas nativas passa
  a incluir também o diretório de compatibilidade do próprio PortMaster
  (`$controlfolder/libs` e `libs.aarch64`), onde firmwares enxutos guardam as suas.
- **FreeType/HarfBuzz de último recurso embarcados.** Firmwares que não trazem nenhuma das
  duas recusavam o port antes de abrir. O pacote agora inclui um par AArch64 sem
  dependências (sem zlib/png/bzip2/brotli/glib/graphite2/icu/cairo, libstdc++ estática,
  GLIBC 2.17), usado **somente** quando o firmware não tem nenhuma. A integração com
  FreeType foi mantida na HarfBuzz porque os bindings do jogo chamam
  `hb_ft_font_create_referenced`.
- **Mesa/Panfrost.** A sonda e o jogo pedem explicitamente o driver ES do SDL
  (`SDL_OPENGL_ES_DRIVER`). Sem isso esses firmwares devolvem um contexto desktop-GL, os
  shaders GLSL ES não compilam e a tela fica preta — ou o device capaz caía no perfil
  ES2/ETC1 por engano.
- **`SDL_VIDEODRIVER` herdado que não existe no device.** Se a sonda não inicializa com o
  backend herdado do frontend mas inicializa sem ele, o port apenas **remove** a variável e
  deixa o SDL escolher. O port continua não nomeando backend nenhum.
- **Áudio em Knulli/Batocera/muOS.** O jogo guarda os saves com `HOME` redirecionado, o que
  escondia o `~/.asoundrc` — justamente onde esses firmwares definem o PCM que funciona e o
  volume por software. O `.asoundrc` do sistema (ou o `asound.conf` do PortMaster) volta a
  ficar visível para o novo `HOME`.
- **Órfão nunca sobrevive ao frontend.** Se o frontend mata o launcher ao sair do menu, um
  `trap` derruba o host que ficaria segurando o framebuffer/DRM master — a causa clássica de
  "a próxima abertura fica preta". Só processos deste port são tocados.
- **Contrato do frontend.** `CUR_TTY` passa a vir do `control.txt` do firmware em vez de ser
  fixo em `/dev/tty0`, e o `log.txt` tem um dono só: o launcher trunca no início da sessão e
  o host não reescreve o arquivo por baixo desse descritor.
- **Build reprodutível.** `global.json` fixa o SDK .NET 9.0.315, então instalar um SDK novo
  na máquina não muda mais o artefato publicado.

## 2.0 RC3
- O instalador agora aceita três fontes legais da Android v1.4.5: APK monolítico
  completo, conjunto completo de splits soltos e bundles `.apks`, `.apkm` ou `.xapk`.
- Base, split ARM64 e splits de dados são montados como uma única árvore virtual. A
  instalação continua exigindo exatamente 25.905 assets, 613 WEMs, tamanhos, CRCs,
  fingerprints e bibliotecas oficiais; conjuntos incompletos, conflitantes ou mistos
  são recusados antes de publicar qualquer dado.
- Bundles são expandidos em staging com limites de quantidade/tamanho, nomes seguros,
  CRC durante a cópia, arquivo temporário e rename atômico. A cópia original do usuário
  é preservada e uma interrupção pode reutilizar somente APKs internos cujo CRC confere.
- O extrator e o bake ETC1/ETC2 agora leem vários APKs diretamente, sem precisar criar
  um APK mesclado gigante. O resume de assets ASTC também confere CRC, não apenas tamanho.
- `StreetsOfRage4.sh` foi reduzido ao contrato PortMaster. Lock/single-instance, perfil,
  preflight, setup, aliases nativos, save/áudio, controles e ciclo do jogo agora ficam
  no modo compilado `sor4host --starter`; `sor4_runtime.sh` saiu do ZIP.
- Mantidos os consertos validados do RC2: configurações/idioma persistentes, Quit nativo,
  aliases FreeType/HarfBuzz e proteção contra o silêncio na prisão da fase 2.

## 2.0 RC2
- Configurações, idioma e opções agora são persistidos em `save/Config.txt`. O patch
  restaura o serializador local que o RC anterior havia desativado e usa `HOME` sem
  caminho fixo de aparelho.
- A opção Quit do próprio jogo agora completa o shutdown de áudio/assets/threads e
  encerra o processo Linux. Select+Start continua disponível como saída de emergência.
- Corrigido o silêncio temporário na prisão: segmentos Wwise minúsculos que contêm apenas
  uma linha de tempo silenciosa não substituem mais a música válida em reprodução.
- FreeType/HarfBuzz aceitam bibliotecas versionadas ou não versionadas e recebem todos
  os aliases usados pelos bindings Android, antes de qualquer extração ou bake.
- Instalações do RC anterior migram atomicamente da receita v2 para v3 sem APK e sem
  reextrair/reconverter os assets. O starter permanece modular, não força drivers SDL,
  não para o frontend e não inicia um segundo mapper por padrão.

## 2.0 RC
- Novo caminho ASTC nativo: 23.745 texturas na resolução original, sem conversão no
  primeiro boot em GPUs compatíveis. O formato `98` específico do jogo foi validado
  como ASTC 6x6 e agora chega corretamente ao GLES.
- Perfis automáticos `1gb`, `2gb` e `high` pela RAM física. No perfil extremo de 639 MiB,
  o pager limita a residência de texturas a 80 MiB e relê somente o nível 0 do XNB.
- Page faults visíveis corrigidos: a textura volta antes do desenho; atlas embutidos de
  SpriteFont permanecem residentes. Golpes, personagens e inimigos não piscam quadrados;
  o contador de faults no teste longo caiu de mais de 61 mil para 253.
- Setup BYO-data transacional, retomável e com rollback. Validação exata da v1.4.5,
  25.905 assets, 613 WEM e manifesto completo nome/tamanho/CRC; o APK agora é preservado.
- Fallback ETC2 para GLES3 sem ASTC e ETC1 para GLES2/Mali-450. Conversão de pouca RAM
  usa um worker, arquivos atômicos e retomada depois de interrupção.
- Sonda real GLES3→GLES2, streaming de música restaurado, resolução automática de
  HarfBuzz em CFWs enxutos e nenhum driver SDL de vídeo/áudio forçado.
- O preflight de FreeType/HarfBuzz agora ocorre antes do bake; a tela de instalação
  também usa sdl2-compat em firmwares cujo backend de vídeo existe apenas no SDL3.
- Launcher reduzido ao ciclo PortMaster e runtime complexo isolado em helper source-only,
  sem mudar perfis, política SDL2/SDL3, áudio, save ou setup transacional.
- Saída limpa: threads, sources, buffers, contexto e device OpenAL são encerrados na
  ordem correta, eliminando o abort `alc_cleanup/LockLists` depois de SELECT+START.
- ZIP/extrator endurecidos contra path traversal, duplicatas, arquivos parciais, bake
  misturado e limites de alocação hostis; artefatos gerenciados são reproduzíveis e sem PDB.
- Testado em jogo até a segunda fase num RK3326/Mali-G31 com 639 MiB de RAM física:
  arte nativa, música/SFX, HUD/barra de vida e combate confirmados.

## 1.1
- **CO-OP LOCAL ate 4 jogadores!** O build mobile fundia todos os controles no Player 1 e
  nao chamava o handler de "entrar" (join). Destravado: cada controle que apertar A na selecao
  de personagem vira um player novo (P2/P3/P4). Validado com 2 (Axel + Blaze na fase, 2 vidas).
- **Fix de crash** no caminho dificuldade -> selecao de personagem (reportado em RG40XXH/MuOS):
  o Wwise sequestrava o sinal que o GC do .NET usa. Roteado pela nossa camada -> sem crash.
- **Barra de vida** corrigida (saia magenta com pontinhos no Mali-450 -> agora verde solida).
- **Performance/logs**: removido o spam de debug que rodava por textura/asset/evento de audio
  (log de sessao caiu de ~milhares de linhas p/ ~15; menos I/O no carregamento).

## 1.0
- Primeiro release. Port NATIVO (MonoGame/.NET 9 CoreCLR + GLES2), BYO-data via APK
  1.4.5; o progressor extrai/patcha/converte (ASTC->ETC1) na 1a execucao e apaga o APK.
- Audio Wwise via reimpl OpenAL: musica, vozes e SFX de combate. Musica troca limpa ao
  mudar de cena (roteamento por HIRC, sem sobreposicao); golpe com volume ajustado.
- Backend de video (fbdev/kmsdrm) e audio (Pulse/PipeWire-Pulse/ALSA) auto-detectados pelo
  device — o launcher nao forca nenhum. Binario unico glibc 2.30 (portavel entre devices).
- Controles via gamepad nativo (SDL); SELECT+START sai do jogo. `sor4.gptk` opcional.
- Pasta `licenses/` com as licencas de todos os componentes redistribuidos.
