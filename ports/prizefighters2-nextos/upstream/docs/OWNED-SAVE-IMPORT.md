# Owner save import / Importação de save do proprietário

This optional tool imports only data exported from a copy of Prizefighters 2
that you legally use on Android. It is not a purchase bypass.

Supported inputs:

- the app-data directory containing `files/Career Saves`, `Custom Fighters`,
  `Custom Gyms` and/or `Custom Leagues`;
- Unity's Android PlayerPrefs XML, normally named
  `com.koalitygame.prizefighters2.v2.playerprefs.xml`;
- an existing `PF2PREF1` `shared-preferences.bin` from this port.

Place the export outside `ports/pf2/home`, for example:

```text
ports/pf2/gamedata/owned-android-save/
├── files/
│   └── Career Saves/
└── shared_prefs/
    └── com.koalitygame.prizefighters2.v2.playerprefs.xml
```

Preview without changing files:

```bash
python3 tools/import_owned_android_save.py \
  --source gamedata/owned-android-save \
  --game-dir . \
  --dry-run
```

Import:

```bash
./import-owned-save.sh
```

Existing affected files are copied to a timestamped directory below
`home/import-backups/` first. Symlinks, special files, ambiguous exports and
oversized trees are rejected.

Resolution, fullscreen and Unity session-identity keys from Android are never
imported. This keeps the handheld's real screen geometry and prevents the
save-dependent zoom regression.

Premium note: the tool copies genuine save/preferences values exactly; it does
not identify or modify a premium key and does not create a receipt. Google Play
tokens can be device/account-bound. Whether a legitimately purchased premium
state transfers is decided by the original game and remains unconfirmed until
tested with an owner export.

---

Esta ferramenta opcional importa somente dados exportados de uma instalação
Android que você usa legalmente. Ela não é um bypass de compra.

Coloque o export em `gamedata/owned-android-save/`, execute primeiro o comando
com `--dry-run` e depois `./import-owned-save.sh`. Os arquivos que seriam
substituídos recebem backup em `home/import-backups/`.

Chaves de resolução, tela cheia e identidade de sessão do Android são
ignoradas para preservar a geometria real do portátil e impedir o retorno do
bug de zoom ligado ao save.

A ferramenta não procura nem altera chave premium e não cria recibo. Se um
estado premium comprado legalmente puder ser transferido, a decisão continua
sendo do jogo original; isso permanece não confirmado até um export legal ser
testado.
