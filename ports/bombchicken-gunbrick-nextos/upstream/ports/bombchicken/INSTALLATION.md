# Installation / Instalação

The public package contains no Bomb Chicken game data. The accepted owner-data
identity is Bomb Chicken Android v44/build 45, package
`com.nitrome.bombchicken`, ABI `arm64-v8a`, APK size **133,951,858 bytes** and
APK SHA-256
`0501f71e90412502dfc7c74a0d81adbe822daa3c11fe8f667c0e4e9e6016b32b`.

## English

1. Extract the complete release ZIP into your firmware's **ports** directory,
   preserving this layout:

   ```text
   <ROMS>/ports/Bomb Chicken.sh
   <ROMS>/ports/bombchicken/
   ├── bombchicken-nextos
   ├── nxport.json
   ├── extractor.json
   ├── nxextract/
   └── gamedata/README.txt
   ```

   `<ROMS>` is the root chosen by your firmware; no particular absolute path
   is required. A PortMaster frontend may place the visible `Bomb Chicken.sh`
   in its normal scripts directory while keeping `bombchicken/` in the ports
   data directory. `Bomb Chicken.sh` contains the complete nxbootstrap 0.6.8
   logic; there is no companion bootstrap or nested launcher.

   To update, extract the complete v1.1.7 ZIP over the same active ports root.
   Existing extracted owner data and saves are preserved. If you moved between
   storage roots, remove the old menu entry from the inactive root explicitly.

2. Put the **APK you legally own** in
   `<ROMS>/ports/bombchicken/gamedata/`. The file name does not matter.

3. Start **Bomb Chicken** from the Ports menu. NXExtract 1.2.6 accepts only the
   exact `com.nitrome.bombchicken` v44/build 45 AArch64 payload, validates all
   native libraries and the complete Unity asset tree, then publishes the
   data atomically. Later starts go directly to the game.

4. Your APK is never deleted. After a successful install you may remove it
   from the card to save space, but retain your lawful copy for reinstalls.

Do not create `run.sh`, unpack the APK by hand or copy Android libraries into
the public ZIP.

### Controls

D-pad/left stick moves; A confirms or places a bomb; B places a bomb; Start
pauses/resumes; the right stick moves the pointer in touch-only menus and R3
clicks. Select + Start exits through focus-lost and native pause.

### Diagnosing a launch

The current attempt is recorded in `bombchicken/log.txt`; the previous attempt
is rotated to `bombchicken/log.prev.txt`. Owner-data installation is recorded
separately in `bombchicken/nxextract.log`. If none of those files changes,
inspect frontend/PortMaster placement and shell execution before assigning the
failure to the game.

A script cannot log before the operating system or frontend executes it. If no
launcher or runtime log exists, start at the frontend/launcher boundary rather
than treating silence as a game crash.

## Português

O pacote público não contém dados de Bomb Chicken. A identidade aceita dos
dados do dono é Bomb Chicken Android v44/build 45, pacote
`com.nitrome.bombchicken`, ABI `arm64-v8a`, APK com **133.951.858 bytes** e
SHA-256
`0501f71e90412502dfc7c74a0d81adbe822daa3c11fe8f667c0e4e9e6016b32b`.

1. Extraia o ZIP completo da release na pasta **ports** do seu firmware,
   preservando a estrutura:

   ```text
   <ROMS>/ports/Bomb Chicken.sh
   <ROMS>/ports/bombchicken/
   ├── bombchicken-nextos
   ├── nxport.json
   ├── extractor.json
   ├── nxextract/
   └── gamedata/README.txt
   ```

   `<ROMS>` é a raiz escolhida pelo firmware; nenhum caminho absoluto
   específico é obrigatório. Um frontend PortMaster pode manter o
   `Bomb Chicken.sh` visível na pasta normal de scripts e `bombchicken/` na
   pasta de dados dos ports. O `Bomb Chicken.sh` contém toda a lógica do
   nxbootstrap 0.6.8; não existe bootstrap auxiliar nem launcher interno.

   Para atualizar, extraia o ZIP v1.1.7 completo sobre a mesma raiz ativa de
   ports. Dados extraídos e saves são preservados. Se você mudou de raiz de
   armazenamento, remova explicitamente a entrada antiga da raiz inativa.

2. Coloque o **APK que você possui legalmente** em
   `<ROMS>/ports/bombchicken/gamedata/`. O nome do arquivo não importa.

3. Abra **Bomb Chicken** no menu Ports. O NXExtract 1.2.6 aceita apenas o
   payload AArch64 exato `com.nitrome.bombchicken` v44/build 45, valida todas
   as bibliotecas e a árvore Unity completa e só então publica os dados
   atomicamente. Nas próximas aberturas o jogo inicia direto.

4. Seu APK nunca é apagado. Depois da instalação você pode removê-lo do cartão
   para liberar espaço, mas guarde sua cópia legal para reinstalações.

Não crie `run.sh`, não extraia o APK manualmente nem coloque bibliotecas
Android no ZIP público.

### Controles

Direcional/analógico esquerdo move; A confirma ou bota bomba; B bota bomba;
Start pausa/despausa; o analógico direito move a seta nos menus touch-only e
R3 clica. Select + Start sai pela perda de foco e pause nativos.

### Diagnóstico de abertura

A tentativa atual fica em `bombchicken/log.txt`; a anterior é movida para
`bombchicken/log.prev.txt`. A instalação dos dados do dono fica separada em
`bombchicken/nxextract.log`. Se nenhum desses arquivos mudar, confira primeiro
o registro/caminho no frontend ou PortMaster e a execução do shell.

Um script não consegue registrar nada antes de o sistema ou frontend executá-lo.
Se não existe log de launcher nem de runtime, comece pela fronteira
frontend/launcher em vez de classificar o silêncio como crash do jogo.
