# Terraria Android loader — PT-BR / English

## Português (Brasil)

Este pacote executa **Terraria Android 1.4.5.6.4**, Unity 2021.3.56f2 IL2CPP,
em portáteis Linux AArch64 por meio de um loader de compatibilidade nativo. Não
é o port FNA, não é emulação Android e não inclui o jogo.

O pacote é BYO-data: coloque seu próprio APK legal em `terraria/gamedata/` e
abra `Terraria`. O NXExtract 1.2.0 valida versão, ABI, tamanhos e hashes antes
de preparar os dados de forma transacional.

### Instalação rápida

1. Extraia o ZIP na pasta de Ports.
2. Confirme que `Terraria.sh` está ao lado da pasta `terraria/`.
3. Coloque o APK legal Android `1.4.5.6.4`, ABI `arm64-v8a`, em
   `terraria/gamedata/`.
4. Abra Terraria e aguarde a preparação da primeira execução.

O firmware deve fornecer Python 3, SDL2, EGL e GLES2. Consulte
`INSTALLATION.md` para atualização e solução de problemas.

### Controles e saída

Menus, inventário e gameplay usam o controle Xbox/InControl original do jogo.
No teclado de nomes, D-pad navega, `A` ou `R3` escolhe, `B` apaga, `X` alterna
maiúsculas/minúsculas, `START` ativa `DONE` e `SELECT` cancela.

Você pode sair de duas formas:

- escolha `Quit Game` dentro do Terraria; ou
- pressione `SELECT+START` juntos para sair imediatamente.

No menu, o loader preserva o fluxo original de salvamento do jogo; no combo,
ele solicita a saída diretamente. Os dois caminhos passam pelo teardown
protegido antes de devolver o controle ao frontend.

### Compatibilidade

O loader público é AArch64, usa SDL2 do firmware e exige no máximo
`GLIBC_2.27`. A interface do NXExtract exige no máximo `GLIBC_2.17`. O caminho
gráfico é escolhido pelas capacidades reais: SDL/KMSDRM usa contexto SDL;
Mali/fbdev mantém EGL do fornecedor.

Validação física completa foi realizada no perfil R36S-class com ArkOS,
Mali-G31/KMSDRM e 640x480. Outros perfis precisam de teste físico próprio.

## English

This package runs **Terraria Android 1.4.5.6.4**, Unity 2021.3.56f2 IL2CPP,
on AArch64 Linux handhelds through a native compatibility loader. It is not the
FNA port, is not Android emulation, and does not include the game.

The package is BYO-data: place your own legal APK in `terraria/gamedata/` and
launch `Terraria`. NXExtract 1.2.0 verifies version, ABI, sizes, and hashes
before preparing the data transactionally.

### Quick installation

1. Extract the ZIP into the Ports directory.
2. Make sure `Terraria.sh` sits next to the `terraria/` directory.
3. Put the legal Android `1.4.5.6.4` APK with the `arm64-v8a` ABI in
   `terraria/gamedata/`.
4. Launch Terraria and let first-run setup finish.

The firmware must provide Python 3, SDL2, EGL, and GLES2. Read
`INSTALLATION.md` for update and troubleshooting details.

### Controls and exit

Menus, inventory, and gameplay use Terraria's original Xbox/InControl path. On
the name keyboard, the D-pad navigates, `A` or `R3` selects, `B` deletes, `X`
toggles case, `START` activates `DONE`, and `SELECT` cancels.

There are two supported ways to exit:

- choose `Quit Game` inside Terraria; or
- press `SELECT+START` together for an immediate exit.

The menu path preserves the game's original save flow, while the combo requests
exit directly. Both paths run the guarded teardown before returning control to
the frontend.

### Compatibility

The public loader is AArch64, uses the firmware's SDL2, and requires at most
`GLIBC_2.27`. The NXExtract UI requires at most `GLIBC_2.17`. Graphics are
selected by actual capability: SDL/KMSDRM uses an SDL-owned context, while
Mali/fbdev retains vendor EGL.

Full physical validation was completed on an R36S-class ArkOS profile with
Mali-G31/KMSDRM at 640x480. Other profiles require their own physical tests.

## Data and licences / Dados e licenças

Supported source: Android package `com.and.games505.TerrariaPaid`, game version
`1.4.5.6.4`, Unity `2021.3.56f2`, ABI `arm64-v8a`.

The APK, Android libraries, Unity data, saves, and logs are not included. The
loader is GPL v3 and NXExtract is MIT licensed. Terraria remains proprietary
to its rightsholders. This project is independent and is not affiliated with
or endorsed by Re-Logic, 505 Games, or Unity Technologies.

Source / código-fonte: https://github.com/NextOs-Ports/terraria-nextos
