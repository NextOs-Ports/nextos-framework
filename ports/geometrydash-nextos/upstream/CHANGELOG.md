# Changelog

## 1.0.2

Rodada de campo com o NextOS na TV (Mali-450) depois dos primeiros relatos do
Discord.

- **Andar nos niveis de plataforma (as missoes da Torre, 2.2).** O jogo deixou
  de ser de um botao so' em 2.2 e o port so' sabia tocar a tela. Agora o D-pad e
  o **analogico esquerdo** andam, pelo caminho do PROPRIO jogo:
  `UILayer::handleKeypress` -> `GJBaseGameLayer::queueButton`, a mesma fila que
  as setas de tela alimentam. Nada e' simulado.
  A instancia da `UILayer` nao e' lida por offset (offset de um binario so' ja'
  congelou nivel aqui, ver `step_guard.c`): a propria `UILayer` se entrega
  trocando-se o slot de `draw()` na vtable dela -- slot ACHADO comparando com o
  endereco de `UILayer::draw`, nunca por indice cravado. "Tem nivel na tela"
  passa a ser "alguma UILayer desenhou nos ultimos 250 ms".
  As teclas sao `A`/`D`, nao as setas: `handleKeypress` manda as setas com a
  flag de player 2 e `A`/`D` com a de player 1 -- igual em nivel solo, certo em
  nivel dual.
- **Pulo nos niveis de plataforma.** La' o jogo desenha as proprias setas no
  canto inferior esquerdo, e o toque do port cai onde a seta de mira estiver
  parada: parada em cima de uma delas, o botao de pulo andava para o lado. O
  pulo passa a ser TECLA enquanto ha' nivel na tela, entao nao depende de onde
  a seta esta'. Toque e tecla nunca vao juntos -- duas pressoes do mesmo botao
  sao dois pulos.
- **R3 sempre toca**, em qualquer lugar, sempre. A regra de quem toca e' o
  BOTAO, nunca um relogio: a primeira tentativa expirava 4 s depois da ultima
  mira e o R3 clicava e soltava sozinho no meio do aperto. O menu de pause de um
  nivel continua sendo apertado -- com R3, que e' o botao que combina com mirar.
- **O SubZero entra igual, e isso foi CONFERIDO simbolo a simbolo** (nao
  assumido): a build 2.2.147 dele tem o mesmo `UILayer::handleKeypress`, o mesmo
  `queueButton` e as mesmas constantes de tecla e numeros de botao. Os niveis
  dele sao todos classicos, entao nada anda; o pulo dentro do nivel passa pela
  mesma tecla. Build que NAO tenha esses simbolos apenas desliga a funcao, com o
  motivo no log, e se comporta como antes.
- **A escrita na vtable pede permissao antes.** `.data.rel.ro` cai no LOAD RW e
  o loader nao aplica RELRO, entao a escrita e' legitima; ainda assim ela e'
  pedida por `mprotect`, e um sistema que recuse desliga a funcao em vez de
  derrubar o jogo.
- **Cada botao do controle aparece UMA vez no log, com o nome que a SDL da' a
  ele.** Pad cujo GUID nao casa com o `gamecontrollerdb` ganha mapping
  automatico, e ai' "o A nao pula" e' impossivel de responder sem isto.
- **O instalador explica o APK BASE de instalacao dividida.** Relato do Discord:
  o `.xapk` do SubZero e' um APK base de 159 MB com SO' os assets mais um
  `config.arm64_v8a.apk` com o codigo; quem descompacta e copia so' o base
  recebia "required payload engine-library was not found" e ia procurar outro
  APK -- o APK dele estava certo. Agora o erro diz que o pacote nao tem `lib/`
  nenhuma, que isso e' so' a parte base de um split, e o que fazer (copiar o
  `.xapk` inteiro ou por o `config.arm64_v8a.apk` junto). Quando ha' `lib/` mas
  da ABI errada, o erro diz qual ABI veio e qual o port precisa. O aviso entrou
  tambem no `README.txt` do `gamedata/`, no `INSTALLATION.md` e no `README.md`.

## 1.0.1

Correcoes da rodada de verificacao contra o checklist multi-device. Tudo medido
no R36S/ArkOS.

- **Trava de instancia unica no PROPRIO BINARIO** (`flock` no executavel).
  Antes existia so' a varredura de `/proc` feita pelo launcher; script morto
  solta a trava enquanto o jogo continua dono do display e do audio.
- **O hook do `game.apk` era pulado ao reinstalar.** O checkpoint era "o
  arquivo existe e tem tamanho plausivel", ja' verdadeiro logo depois de copiar
  o APK cru, porque entrada e saida tem o mesmo nome. Um `--force-source`
  publicava o pacote original, com parte das entradas em DEFLATE, e o leitor de
  audio do loader so' enxerga STORED: som sumindo depois, sem erro. O hook
  agora grava um carimbo de conteudo fixo e o checkpoint confere o sha256 dele.
- **Build reproduzivel.** O diretorio temporario entrava no simbolo de arquivo
  do objeto em assembly, entao a mesma fonte saia com sha256 diferente a cada
  build. O binario da release passa a ser conferivel contra a fonte.
- **`audit-portability.sh` roda dentro do empacotador**, com as excecoes
  escritas junto do contrato de cada uma em `package/audit-allow.txt`.
- O evdev e' reaberto quando um controle e' plugado com o jogo ja' rodando:
  SELECT/START/L3/R3 desse pad ficavam mortos nos aparelhos que so' os entregam
  como `BTN_TRIGGER_HAPPY`.
- **Audio: `dummy` e `disk` deixam de contar como sucesso.** Os dois ABREM e
  devolvem sucesso, e o jogo seguia mudo achando que tinha som; agora a escada
  de drivers continua ate' achar um que produza audio de verdade.
- **`PULSE_SERVER` herdado e morto nao emudece mais o jogo.** O launcher confere
  se o socket existe; se nao existir, procura o real -- inclusive em
  `$XDG_RUNTIME_DIR/pulse/native`, que e' onde o pipewire-pulse do ROCKNIX
  publica -- e, na falta de qualquer um, devolve a escolha para a autodeteccao
  da SDL em vez de insistir num servidor morto. Nenhum backend e' forcado.
- **Receitas mais tolerantes a builds diferentes.** A identidade forte continua
  sendo o pacote Android e a lib nativa arm64; o que era faixa estreita saiu.
  Antes se exigia `assets/sfx`, `assets/levels` e `assets/icons` e um piso de
  90 MB -- isso reprovaria o APK legitimo de uma build que reorganizasse pastas
  ou de uma versao mais antiga e menor. Ficou o unico caminho que toda build da
  familia tem: `assets/GJ_GameSheet.plist`.
- O empacotador tambem sabe montar um ZIP unico com os dois jogos
  (`package/build-package.sh ambos`); a release continua com um ZIP por jogo.

## 1.0.0

Primeira release: **Geometry Dash** e **Geometry Dash SubZero** num unico
repositorio, com o mesmo nucleo de loader.

- so-loader AArch64 para Cocos2d-x 2.x: JNI, OpenSL ES, EGL, preferencias,
  ciclo de vida e toque servidos em C; a engine original roda sem emulacao.
- Executavel publico com teto **GLIBC 2.30**, dependendo apenas de SDL2,
  GLESv2, EGL, FreeType e libc do firmware.
- **BYO-data pelo NXExtract 1.2.4**: o pacote nao carrega dado de jogo nenhum.
  O APK do usuario e' reconhecido pelo conteudo, fixado por pacote Android
  (cada port aceita so' o seu jogo) e normalizado num `game.apk` STORED que o
  loader le' por minizip. O APK original nunca e' apagado nem alterado.
- Controle **nativo**, sem mapper por cima: SDL GameController mais leitura do
  evdev para SELECT/START/L3/R3, que em varios portateis chegam como
  `BTN_TRIGGER_HAPPY` e ficam fora de qualquer mapping da SDL.
- Save proprio em `userdata/` (o Geometry Dash monta `/data/data/<pkg>/`
  sozinho, em vez de perguntar o caminho gravavel).
- Validado fisicamente em R36S/ArkOS (Mali-G31, glibc 2.30) e em NextOS Elite
  (Mali-450, glibc 2.43): 60 fps, audio, controle e saida limpa nos dois.
