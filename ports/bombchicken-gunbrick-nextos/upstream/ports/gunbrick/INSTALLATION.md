# Installation / Instalação

## English

1. Extract the complete `gunbrick.zip` into the firmware's `ports` directory.
   Keep this layout:

   ```text
   <ROMS>/ports/Gunbrick.sh
   <ROMS>/ports/gunbrick/
   ├── gunbrick-nextos
   ├── nxport.json
   ├── extractor.json
   ├── nxextract/
   └── gamedata/README.txt
   ```

2. Copy a compatible, lawfully obtained Gunbrick Reloaded v10 ARM64 APK into
   `<ROMS>/ports/gunbrick/gamedata/`. Do not unpack or rename its contents.

3. Start **Gunbrick Reloaded** from the ports menu. The clean NXExtract screen
   verifies and installs the exact payload on first launch. Keep the device on
   until it reports success; later launches use the validated marker and do not
   repeat extraction.

NXExtract preserves the APK. A wrong or incomplete version is rejected before
the existing working `assets/` and `lib/` trees are replaced. Consult
`nxextract.log` and `log.txt` inside `gunbrick/` if setup or launch fails.

## Português

1. Extraia o `gunbrick.zip` completo na pasta `ports`, mantendo o launcher
   `Gunbrick.sh` na raiz e a pasta `gunbrick/` conforme a árvore acima.

2. Copie um APK ARM64 v10 compatível e obtido legalmente para
   `<ROMS>/ports/gunbrick/gamedata/`. Não descompacte nem altere seus arquivos.

3. Abra **Gunbrick Reloaded** no menu. A tela limpa do NXExtract valida e
   instala o payload exato no primeiro uso. Aguarde a mensagem de sucesso;
   lançamentos posteriores usam o marcador validado e não repetem a extração.

O APK é preservado. Uma versão errada ou incompleta é recusada antes de trocar
uma instalação funcional. Em caso de erro, consulte `nxextract.log` e
`log.txt` dentro de `gunbrick/`.
