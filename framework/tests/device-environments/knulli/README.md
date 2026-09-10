# Fixture Knulli/Batocera — viewport

Cobre **somente viewport**, a partir da imagem oficial local. Ordem de botões,
mapeamento e a saída por SELECT+START já têm gate próprio e **não** são
duplicados aqui.

A imagem nunca entra no Git e nunca é redistribuída. A leitura é somente
leitura: `mcopy` no deslocamento da partição de boot e `unsquashfs` dos
caminhos declarados. Nada é montado, nada usa loop device, nada define modo de
vídeo e nada toca em DRM, Mali, framebuffer, áudio ou input.

## Preparar e verificar

```sh
python3 framework/tests/device-environments/knulli/prepare.py \
  --image /caminho/para/knulli-h700-...img \
  --output /caminho/para/firmware-environments/knulli-20250813

python3 framework/tests/device-environments/knulli/verify.py \
  --environment /caminho/para/firmware-environments/knulli-20250813
```

## O que fica provado

Da imagem, sobre o que a firmware **declara**:

- a placa (`/boot/batocera.board`) e a autoridade de resolução
  (`/usr/bin/batocera-resolution`);
- as geometrias internas por placa e os modos HDMI que essa autoridade usa;
- que o viewport corrente é uma **consulta em tempo de execução**, não uma
  constante;
- que a rotação (`display.rotate`, valores 0–3) é ofertada **comentada**: a
  firmware não força rotação;
- que a própria autoridade imprime largura e altura em ordens diferentes
  conforme a placa — portanto um par reportado **não** é prova de orientação.

Desses fatos medidos, o gate submete cada geometria ao seletor puro do nxgl e
prova a política:

- painel **quadrado** (720×720) é aceito literalmente, sem correção de aspecto;
- painel **retrato** (480×640) é aceito literalmente, sem troca de eixos;
- modo HDMI conhecido pelo SDL vence o framebuffer, na ordem já contratada;
- sem nenhuma fonte, o resultado é **erro explícito** — resolução nunca é
  inventada.

## Fronteira

O que se prova aqui é de host: leitura do que a firmware declara e a política
pura do framework diante desses números. Painel real, rotação aplicada,
compositor e DRM continuam exigindo prova física separada.
