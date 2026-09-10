# 2.0.1 — 26/08/2026

- **dArkOSRE/RK3326 volta a abrir sem variável manual.** Algumas imagens
  oferecem um dispatcher EGL versionado sem driver, enquanto o Mali-G31
  funcional está nos nomes portáveis `libEGL.so` e `libGLESv2.so`. O runtime
  tenta primeiro a pilha da firmware; somente se todas as combinações
  SDL/KMSDRM falharem, e se firmware/usuário não tiverem escolhido providers,
  encerra o vídeo e faz um único `exec` limpo com o par portável. Não nasce uma
  segunda instância e o fluxo Unity/Android recomeça inteiro na ordem nativa.
- A decisão vem da mesma recuperação canônica já provada pela NXSplash 0.1.2
  no próprio boot. Mali-450/fbdev e pilhas KMSDRM que abrem normalmente não
  entram no reparo.
- Os perfis de memória/desempenho `auto`, `low`, `medium` e `high` permanecem
  intactos. No aparelho de 1 GB, `auto` continua escolhendo `low`: render
  320×240 com upscale pixel-perfect para 640×480, texturas limitadas a 512 e
  RGBA4444.
- Release reconstruída e auditada a partir da tag imutável
  `framework-v2-onda1` (nxbootstrap 0.6.30, NXExtract 1.2.18, nxgl 0.2.14 e
  nxrelease 0.2.30).

# 2.0.0 — 24/08/2026

Port renascido no **framework v2** do NextOS. Tratado como port novo: o
`run.sh` deixou de existir.

- **Launcher único gerado pelo nxbootstrap 0.6.30** (lock, logs, extração,
  handoff do PortMaster, recibo de falha pré-runtime) no lugar do wrapper +
  `run.sh` da fase 1. A lógica por aparelho mora em `port-env.sh`.
- **Perfis de desempenho** (`OCEANHORN_PROFILE`: auto/low/medium/high):
  `low` — padrão automático em ~1 GB — render na metade da resolução com
  upscale inteiro pixel-perfect (o remédio do fill-rate) + textura 2x
  (teto 512 + RGBA4444); `medium` = textura 1,5x (teto 768), render nativo;
  `high` = tudo nativo.
- **Render reduzido no perfil low, agora certo nos DOIS caminhos.** No
  ES3/KMSDRM a Unity não passa pelos wrappers de viewport (vp=0 medido) e
  ficava meio-adaptada — a tela virava um quadrado de 1/4. Conserto = **modo
  LO-REPORT**: todas as superfícies de consulta (ANativeWindow,
  DisplayMetrics, eglQuerySurface) respondem a resolução BAIXA desde o 1º
  frame; a engine desenha coerente no FBO lo e o blit sobe pixel-perfect
  para o painel real (título fullscreen 100,0% dos blocos 2x2 idênticos,
  provado por captura no GO-Super). No fbdev/Mali-450 permanece o modo
  legado (engine na resolução cheia + escala de viewport), que é o provado
  lá. Mais: tamanho do painel real (não mais 1280x720 cravado), NEAREST em
  divisor inteiro (`CUP_RS_FILTER=linear` volta ao suavizado).
- **Pin de GPU Mali-450 no binário** (`gpu_perf.c`): todos os pixel
  processors no nível oficial máximo + guarda térmica, com restauração
  garantida na saída (antes era o `run.sh` que fazia e podia não restaurar).
- **NXExtract 1.2.18** (motor+UI canônicos do v2) e **frame proof nxgl
  0.2.14** (recibo de lançamento + veredito nos dois caminhos de present).
- **nxrelease 0.2.30**: pacote determinístico, FRAMEWORK-PIN dos 15
  componentes, SBOM, auditoria PortMaster/HarbourMaster.
- Loader carrega as libs direto de `lib/` (morreu a cópia para o root que o
  FAT-sem-symlink exigia) e continua glibc ≤ 2.27 (build reprodutível em
  container pinado, sem rede).
- Sem swap: o port nunca cria swapfile (regra da casa; a economia de RAM vem
  do perfil low). Sem GC forçado (o GCEVERY crashava no campo).
- Herda os quatro cortes de memória do 1.0.7 (nunca lançado): falta de
  memória chegava como violação de acesso.

# Changelog — Oceanhorn: Chronos Dungeon (NextOS Ports)

## v1.0.7 (Universal) — 05/08/2026

Relato de campo do Ivan (ArkOS/AeUX, aparelho de 1 GB, `tutorial trigger hit` →
`signal 11 SEGV_ACCERR`). O log entregou a causa inteira. Obrigado pelo relato.

### Corrigido
- **Crash ao entrar no tutorial em aparelho de 1 GB** — o jogo morria com
  `SIGSEGV (SEGV_ACCERR)` dentro do alocador principal da Unity
  (`ALLOC_DEFAULT_MAIN`), num endereço que caía numa reserva de memória ainda
  **protegida** (`---p`), no fim de um bloco de 16 MB. O mecanismo: a Unity
  reserva regiões grandes com `mmap(PROT_NONE)` e depois "commita" pedaços com
  `mprotect(RW)`. Com a memória do aparelho no fim (`MemAvailable` real de
  26 MB dois segundos antes do crash) esse `mprotect` falha — e o alocador
  escreve mesmo assim, dentro da reserva que nunca foi liberada para escrita.
  Não era corrupção nem endereço de outra build: era falta de memória chegando
  no motor como violação de acesso. Quatro correções entram juntas, e nenhuma
  delas faz coisa alguma num aparelho com folga:
  - **O motor voltou a enxergar a memória real.** O port entrega um
    `/proc/meminfo` próprio para a Unity, e o valor LIVRE estava congelado em
    256 MB desde sempre. O motor nunca sentia aperto nenhum: seguia crescendo
    até o commit falhar de verdade. Agora o livre acompanha o aparelho, com o
    mesmo teto de 256 MB de antes — enquanto há folga o motor lê exatamente o
    número de sempre, e o alvo já validado não muda. `MemTotal` continua nos
    512 MB conservadores que dimensionam os caches.
  - **Commit sob demanda como rede de segurança.** Quando a escrita cai numa
    reserva anônima ainda protegida, o port completa o commit da janela que
    faltou e repete a instrução, em vez de morrer. O pedido de 16 MB de uma vez
    é o que não passa; uma janela pequena passa. Escopo estreito de propósito
    (só área `---p` anônima de 1 MB ou mais, com teto de RAM resgatada), e
    guard page de pilha continua sendo crash de verdade.
  - **Aviso de memória pela TAXA de queda, não só pelo nível.** O log de campo
    mostra a memória caindo de 113 MB para 26 MB entre dois sinais, dentro de
    um carregamento — o aviso pelo nível chegava tarde demais e só voltava a
    tocar dez segundos depois. Agora um carregamento que come dezenas de MB por
    segundo dispara o corte enquanto ainda há folga, e o sinal se repete a cada
    3 s enquanto durar. Aparelho em regime normal fica parado num patamar e
    nunca dispara essa regra.
  - **Música inteira na RAM: o teto existia mas nunca reprovava.** O limite de
    16 MB por clipe residente lia o tamanho declarado por um caminho que, fora
    do handler de crash, respondia sempre zero — na prática todo clipe de
    streaming virava residente, o arquivo inteiro em memória. O teto voltou a
    funcionar, e sob pressão de memória nenhuma conversão acontece: o clipe
    segue em stream.

### Notas
- Nada muda em dados, saves ou instalação: basta trocar os arquivos do port.
- Válvulas de escape, se algum aparelho preferir o comportamento antigo:
  `OCEAN_FAKE_MEMINFO=1` (livre fixo em 256 MB, como na v1.0.6),
  `OCEAN_NO_COMMIT_RESCUE=1` (sem commit sob demanda),
  `OCEAN_LOWMEM_FALL_KB` / `OCEAN_LOWMEM_EARLY_KB` (regra de taxa),
  `OCEAN_RESIDENT_MAX_MB` (teto do clipe residente).
- A linha `[MEM]` do log agora traz também a queda por segundo — é ela que diz,
  no próximo relato, quanto uma cena custa de verdade.

## v1.0.6 (Universal) — 04/08/2026

Duas causas grandes caíram nesta versão — as duas reproduzidas, rastreadas até
a raiz e validadas fisicamente no R36S. Obrigado aos dois relatos de campo
(dArkOSRE e MagicX Zero 28/Knulli): foram eles que fecharam o diagnóstico.

### Corrigido
- **Tela preta com som ao entrar na caverna (e em qualquer troca de nível com
  anúncio)** — nunca foi memória. Ao trocar de nível o jogo pede um anúncio
  intersticial, que antes exige o consentimento de privacidade do Google (UMP).
  Nada desse Java existe fora do Android real, e o shim respondia `null` a toda
  criação de objeto: o pedido de consentimento ficava pendente para sempre
  (`Consent information loaded` nunca aparecia em log nenhum) e a cutscene
  esperava eternamente. Em aparelho sem swap, o OOM killer terminava o processo
  em ~2 minutos — o que fazia parecer falta de memória. O shim agora entrega
  objetos reais para o stack de anúncios inteiro (UMP, GoogleMobileAds,
  mediações), responde as consultas de consentimento e entrega os callbacks que
  o SDK entregaria: consentimento resolvido na hora, e "anúncio falhou ao
  carregar" quando o jogo pede um. O jogo então segue o próprio caminho — sem
  anúncio para mostrar — e a cutscene continua.
- **Controle "fantasma" (personagem que continua andando sozinho, ação que
  continua repetindo)** — defeito estrutural na injeção de input, presente
  desde a v1.0.0 em todos os aparelhos. O motor lê parte do evento DURANTE a
  injeção e o resto DEPOIS, da fila de input (provado com contador de leituras
  no aparelho). Todos os eventos compartilhavam um único objeto com estado
  global: quando dois eventos saíam próximos — soltar uma diagonal, trocar de
  direção rápido —, a leitura adiada do primeiro devolvia os campos do
  segundo. A soltura da direção literalmente virava outro evento no meio do
  caminho, e o jogo nunca ficava sabendo que o stick voltou ao centro. Agora
  cada evento injetado congela os próprios campos num snapshot e os getters
  respondem pelo evento certo. Validado no aparelho: girar e soltar o
  analógico para o movimento imediatamente.
- Robustez adicional do mesmo pente-fino de input: `downTime` por botão (o UP
  de uma tecla saía carimbado com o tempo de outra), estado dos eixos
  reafirmado periodicamente e soltura ecoada (um evento perdido numa engasgada
  custa meio segundo, não a sessão), direção presa na faixa morta da histerese
  expira em ~0,75 s (stick com repouso deslocado não anda mais sozinho), sonda
  de SELECT/START confere o nome do dispositivo (com dois controles, não lê
  mais o aparelho errado) e SELECT+START solta todos os botões antes de sair.
- **Instalação completa reprovada na faxina, em firmware com `/userdata` exFAT
  (Knulli, Batocera)** — o jogo terminava de instalar, o payload era validado e
  gravado, e só então o extrator falhava ao apagar o próprio cache temporário:
  `[Errno 39] Directory not empty` em `source-cache/bundle-*`, e o launcher
  reportava erro de instalação de dados com o jogo inteiro no lugar. Em
  sistema de arquivos montado por FUSE — exFAT, NFS, SMB — apagar arquivo que
  ainda está aberto não remove a entrada: deixa um marcador escondido, e o
  diretório se recusa a sair. O NXExtract 1.2.1 fecha todos os arquivos de
  origem antes da limpeza e, se o sistema de arquivos ainda recusar, registra o
  aviso e deixa o cache para a próxima execução em vez de abortar. Sobra de
  cache é inofensiva; instalação boa reprovada não é. Em ext4 (ArkOS, ROCKNIX)
  o defeito era invisível. Achado por relato contra o Horizon Chase no Knulli e
  corrigido em todos os ports que compartilham o extrator.

### Notas
- v1.0.4 e v1.0.5 continuam publicadas; nada de compatibilidade mudou. Dados,
  saves e instalação são os mesmos — basta trocar os arquivos do port.
- `OCEAN_INPUTLOG=1` agora também imprime os contadores de leitura de evento
  (a instrumentação que fechou este diagnóstico).

## v1.0.5 (Universal) — 04/08/2026

Terceira rodada de log de campo (R36S/dArkOSRE, tela preta com som ao entrar na
caverna e encerramento em ~2 minutos). O log entregou duas causas independentes,
as duas corrigidas aqui. Obrigado pelo relato.

### Corrigido
- **Patch interno aplicado às cegas numa build diferente do jogo**: o log do
  testador é de uma libunity **2022.3.62f1**; a build de referência do port é a
  **2022.3.61f1**. Endereço interno é válido só para a build de onde foi
  extraído — e o do wrapper `createSound` caía em **outra função**, que era
  sobrescrita com o desvio. Efeito visível: o fallback de streaming de áudio
  nunca rodava (daí os `Cannot create FMOD::Sound instance` e os
  `Failed getting load state of FSB`) e uma função desconhecida ficava
  corrompida. Agora todo sítio de patch **confere a assinatura do código antes
  de escrever**; se o endereço não bate, a mesma sequência é procurada no módulo
  e só é aceita quando aparece **uma única vez**; sem casamento único o patch
  simplesmente não é instalado. Na build de referência o resultado é idêntico ao
  da v1.0.4, instrução por instrução.
- **OOM ao carregar a cena seguinte (a caverna)**: o encerramento era o OOM
  killer. O aviso de memória só saía a 29 MB de `MemAvailable` — depois do
  precipício — e **uma única vez**, porque o rearme exigia a memória voltar a
  subir, o que nunca acontece durante um carregamento. Agora a amostragem é de
  1 s, o aviso sai bem antes (`MemAvailable` < 120 MB), ele **se repete** a cada
  ~10 s enquanto durar a pressão, e cada aviso devolve ao kernel as arenas
  livres da glibc. Nenhum swap é criado e nada no sistema do usuário é alterado.
- **Teto para o fallback de áudio residente**: um clip de streaming convertido
  em residente ocupa a RAM inteira do clip. Acima de 16 MB (ajustável) ele
  continua no caminho de stream original.

### Adicionado
- Linha `[BUILD]` no início do log dizendo se a sua cópia do jogo casa com a
  build de referência — qualquer relato futuro já vem com essa resposta.
- Curva de memória no log sem precisar de variável de diagnóstico: `[MEM]` a
  cada ~60 s, e a cada ~5 s enquanto houver pressão.
- `userdata/ocean-env.sh`: o launcher carrega esse arquivo se ele existir. É o
  lugar para experimentar uma opção sem editar arquivo distribuído, e ele
  sobrevive à atualização do port.
- Opções novas: `OCEAN_LOWMEM_KB`, `OCEAN_RESIDENT_MAX_MB` e
  `OCEAN_TRUE_MEMINFO=1` (esta última mostra ao motor a memória livre real do
  aparelho, em vez de um valor fixo — **opt-in**, por mudar o comportamento do
  motor no alvo já validado).

### Sobre a caverna
As duas correções acima atacam exatamente o que o log prova. A confirmação de
que a caverna carrega precisa de uma partida no aparelho: se ainda faltar
memória, o novo log traz a curva `[MEM]` do carregamento inteiro, e o primeiro
knob a experimentar é `OCEAN_TRUE_MEMINFO=1`.

## v1.0.4 (Universal) — 03/08/2026

Segunda rodada de logs de campo. Obrigado de novo, testadores — os três
relatos viraram três correções.

### Corrigido
- **Crash aos ~60 segundos (muOS, e provável nos demais)**: era o nosso GC
  periódico forçado da v1.0.3 — o log de campo mostra o SIGSEGV logo após a
  "limpeza". Removido. A pressão de memória agora usa o caminho Android
  legítimo: quando `MemAvailable` aperta, o port chama `nativeLowMemory` — o
  mesmo sinal que o Android real manda — e a Unity solta caches por conta.
- **Imagem borrada (dArkOSRE)**: era o teto de textura 384. Com o RGBA4444 o
  teto volta a 512, que gasta MENOS RAM que o 384 em 32-bit e fica nítido.
- **Direção "presa" (anda pra cima sozinho)**: a histerese do analógico
  travava com stick gasto (soltava só abaixo de 0,22; um stick descansando em
  0,25 mantinha a direção engajada para sempre). Novos limiares 0,40/0,30, e
  as trocas de direção agora injetam o RELEASE antes do PRESS — sem diagonal
  fantasma.
- **Toque rápido perdido (trocar de herói "não pegava")**: um aperto mais
  curto que um frame de 30 Hz caía entre dois polls e sumia. Os eventos do SDL
  agora alimentam um latch que garante o toque no frame seguinte.

## v1.0.3 (Universal) — 03/08/2026

Rodar em 1 GB de verdade — por economia no binário, nunca mexendo no sistema
de ninguém.

### Adicionado
- **CUP_TEX16**: atlas estático RGBA8 com 1024+ num eixo sobe para a GPU como
  **RGBA4444** — metade da RAM de textura com a resolução da arte intacta
  (pixel art usa alpha duro, que 4 bits preservam). Automático em aparelho DRM
  com <1,25 GB; render targets, sRGB e texturas comprimidas ficam intactos.
  O Mali-450/fbdev não muda em nada.

### Corrigido
- Nenhum swap é criado, em nenhuma hipótese (removido no mesmo dia em que
  entrou; era prática de PortMaster antigo e não volta).

## v1.0.2 (Universal) — 03/08/2026

Correções guiadas pelos primeiros logs de campo (obrigado, testadores!).

### Corrigido
- **ROCKNIX/Panfrost (RG-DS): abortava com "[EGL] Unable to find a configuration
  matching minimum spec"** — dois defeitos combinados:
  - PulseAudio herdado morto derrubava o `SDL_Init(VIDEO|AUDIO)` inteiro; vídeo
    e áudio agora inicializam separados, com retry sem o driver herdado (o
    mesmo fix validado no TASM2 v1.1.7 e no Horizon Chase v1.0.3).
  - O Mesa devolvia RGBX8888 e este Unity exige RGBA8888 exato; adotado o
    contrato de EGLConfig do Horizon Chase v1.0.3 (seleção e relato do config
    real RGBA de 8 bits por canal).
- **dArkOSRE (R36S): jogo encerrado do nada em gameplay (`Killed`)** — era o
  OOM killer num firmware sem swap. Resolvido por ECONOMIA, sem tocar no
  sistema do usuário: teto de textura mais agressivo (384) quando o firmware
  não oferece swap nenhum, e GC periódico do IL2CPP (`CUP_GCEVERY=900`) em
  aparelhos de ~1 GB para conter o heap gerenciado. Nenhum swap é criado.
- **muOS (RG 40XX-H): wrapper falhava sem deixar rastro** — o wrapper agora
  registra `oceanhorn-wrapper.log` ao lado de si mesmo com os caminhos
  testados, mostra o erro na tela, e conhece as raízes de ROMs do muOS
  (`/mnt/mmc`, `/mnt/sdcard`) e do Batocera (`/userdata`).
- Varredura de instâncias não polui mais o log com corridas de `/proc`.

## v1.0.1 (Universal) — 03/08/2026

Correção crítica de campo: a v1.0.0 falhava em silêncio absoluto fora dos
aparelhos de desenvolvimento — sem nenhum log e sem nenhuma mensagem na tela.

### Corrigido
- **Logs**: todo o ciclo (extrator, launcher e jogo) agora é gravado sozinho em
  `ports/oceanhorn/launcher.log` (rotacionado em `launcher.prev.log`). A v1.0.0
  não redirecionava nada quando lançada pelo menu.
- **Erros visíveis**: falha fatal agora aparece na tela do aparelho (`CUR_TTY`)
  com instrução do que fazer, em vez de voltar ao menu como "tela preta".
- **`pm_platform_helper`**: o helper do PortMaster que faz o frontend de cada
  CFW entregar o display agora é chamado antes do jogo (padrão Prizefighters 2).
  Sem ele, em vários firmwares o jogo rodava atrás de uma tela preta.
- Console visível é limpo antes da primeira cena (padrão Terraria).
- `python3` ausente vira mensagem clara em vez de falha muda.
- Ajustes de malloc para aparelhos com pouca RAM (padrão PF2).

## v1.0.0 (Universal) — 03/08/2026

Primeira versão multi-device.

### Adicionado
- Runtime público único exigindo apenas **GLIBC_2.27**, construído no Debian
  Buster, com o teto de 2.30 verificado pelo próprio build.
- Launcher universal: PortMaster opcional, wrapper visível fino, escopo de
  bibliotecas do firmware, seleção de runtime e ciclo de vida supervisionado.
- `SELECT+START` em pads sem `BTN_SELECT`/`BTN_START` físicos (RG351/R36S), com
  os ordinais calculados do bitmap do próprio pad.
- Medidor opcional de nível de áudio (`OCEAN_AUDIOLOG=N`) para auditar som sem
  depender de ouvir o aparelho.

### Alterado
- `CUP_ALPHAFIX` deixou de ser global: liga apenas no caminho fbdev, onde foi
  comprovado. Em KMSDRM o jogo desenha correto sem ele.
- Perfil de textura escolhido por RAM e capacidades reais, não por modelo.

### Validado fisicamente
- **R36S / ArkOS** (Mali-G31, KMSDRM 640×480): título, cutscene e dungeon
  jogável, áudio, controle e saída limpa. Pico de 352 MB de RSS.
- **NextOS Elite / Amlogic-old** (Mali-450, fbdev 1280×720): sem regressão, com
  o mesmo binário universal.
