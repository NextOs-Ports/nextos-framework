# Instalação / Installation

## Português (Brasil)

### Requisitos

- Linux AArch64/ARM64 com Python 3, SDL2, EGL e GLES2;
- APK legal Terraria Android `1.4.5.6.4`, pacote
  `com.and.games505.TerrariaPaid`, ABI `arm64-v8a`;
- espaço livre na mesma partição do port. O NXExtract mantém uma margem de
  segurança de 256 MiB além dos dados temporários.

### Instalação nova

1. Extraia o ZIP diretamente na pasta de Ports do firmware.
2. Deixe `Terraria.sh` e a pasta `terraria/` lado a lado.
3. Copie o APK para `terraria/gamedata/`; o nome do arquivo não importa.
4. Atualize a lista de ports, se necessário, e abra Terraria.
5. Aguarde o NXExtract validar e preparar os dados na primeira execução.

```text
ports/
├── Terraria.sh
└── terraria/
    ├── terraria
    ├── run.sh
    ├── extractor.json
    └── gamedata/
        └── seu-apk-legal-1.4.5.6.4.apk
```

### Atualização

Saia do jogo, confirme que não há outra instância aberta e faça backup de
`terraria/Players/` e `terraria/Worlds/`. Extraia o ZIP novo por cima,
preservando `gamedata/`, `Players/` e `Worlds/`.

### Saída

Use `Quit Game` no próprio menu ou pressione `SELECT+START` juntos. Não é
necessário segurar o combo. Ambos os caminhos retornam ao frontend.

### Problemas comuns

- APK não encontrado: confirme o `.apk` em `terraria/gamedata/`.
- Versão incompatível: use exatamente `1.4.5.6.4`; renomear outro APK não
  altera seus hashes.
- Pouco espaço: libere espaço na partição do port; a preparação usa estágio
  temporário e uma margem de segurança de 256 MiB.
- Falha no boot: consulte `terraria/run.log` e `terraria/nxextract.log`, mas
  revise dados locais antes de compartilhar logs.

## English

### Requirements

- AArch64/ARM64 Linux with Python 3, SDL2, EGL, and GLES2;
- legal Terraria Android `1.4.5.6.4` APK, package
  `com.and.games505.TerrariaPaid`, ABI `arm64-v8a`;
- free space on the port filesystem. NXExtract keeps a 256 MiB safety margin
  in addition to temporary data.

### New installation

1. Extract the ZIP directly into the firmware's Ports directory.
2. Keep `Terraria.sh` and the `terraria/` directory side by side.
3. Copy the APK into `terraria/gamedata/`; its filename does not matter.
4. Refresh the ports list if required, then launch Terraria.
5. Let NXExtract verify and prepare the data on the first run.

### Update

Exit the game, make sure no other instance is running, and back up
`terraria/Players/` and `terraria/Worlds/`. Extract the new ZIP over the old
port while preserving `gamedata/`, `Players/`, and `Worlds/`.

### Exit

Use `Quit Game` in Terraria's menu or press `SELECT+START` together. No hold is
required. Both paths return to the frontend.

### Common problems

- APK not found: check for a `.apk` file in `terraria/gamedata/`.
- Incompatible version: use `1.4.5.6.4`. Any Play Store build of that version
  is accepted (single APK, splits, or an AntiSplit-M merge) as long as it
  carries the arm64-v8a libraries; renaming an APK of another version does
  not make it compatible.
- Low storage: free space on the port filesystem; setup uses a temporary stage
  plus a 256 MiB safety margin.
- Boot failure: read `terraria/run.log` and `terraria/nxextract.log`, but review
  local details before sharing logs.
