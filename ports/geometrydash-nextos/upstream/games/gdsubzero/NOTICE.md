# NOTICE

## O que este projeto distribui

Somente codigo proprio: o so-loader em C, os shims de plataforma, os scripts de
launcher/empacotamento e a copia embutida do NXExtract (MIT, veja
`licenses/NXExtract-MIT.txt`).

## O que este projeto NAO distribui

**Nenhum dado de jogo.** Nao ha' APK, asset, sprite, musica, efeito sonoro,
fonte, nivel nem biblioteca nativa de terceiro neste repositorio ou nos ZIPs de
release. Em particular, nao acompanham o download:

- `libcocos2dcpp.so` e `libfmod.so` (bibliotecas nativas dos jogos);
- qualquer arquivo de `assets/` (`.plist`, `.png`, `.mp3`, `.ogg`, niveis);
- o `game.apk` usado em tempo de execucao — ele e' montado NO APARELHO, a
  partir do APK do proprio usuario.

O empacotador recusa a release se qualquer um desses arquivos aparecer na
arvore.

## Marcas e direitos

**Geometry Dash** e **Geometry Dash SubZero** sao obras e marcas de **RobTop
Games AB**. Este projeto nao e' afiliado, patrocinado nem endossado pela RobTop
Games. FMOD e' da Firelight Technologies Pty Ltd; Cocos2d-x e' dos seus
respectivos autores. Todos os direitos permanecem com os seus titulares.

O port funciona apenas com o APK que a pessoa que joga ja' possui, comprado na
Google Play. Nada aqui contorna DRM, licenciamento ou verificacao de compra:
o LVL do Android e' um componente Java que simplesmente nao existe fora do
Android, e o so-loader carrega o codigo nativo, nao o aplicativo Android.

## Contato

Problemas, correcoes e relatos de compatibilidade: pelas issues do repositorio.
