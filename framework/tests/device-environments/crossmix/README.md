# Ambiente TrimUI/CrossMix no PC

Reproduz no PC o que a distribuição **oficial** do CrossMix entrega no cartão.
Esta CFW não vem como imagem de disco: vem como a árvore do cartão num ZIP —
o contrato do ambiente é o mesmo, com tamanho e SHA-256 do arquivo oficial,
preparação transacional fora do repositório e recibo de proveniência.

A distribuição nunca entra no Git e nunca é redistribuída.

## Preparar e verificar

```sh
python3 framework/tests/device-environments/crossmix/prepare.py \
  --archive /caminho/para/CrossMix-OS_v1.3.0.zip \
  --output /caminho/para/firmware-environments/crossmix-v1.3.0

python3 framework/tests/device-environments/crossmix/verify.py \
  --environment /caminho/para/firmware-environments/crossmix-v1.3.0
```

## O que fica provado

- **raízes do cartão** que a própria firmware declara no `control.txt`:
  a pasta do PortMaster e a pasta de dados, ambas contidas na raiz do cartão;
- **bibliotecas**: as que a firmware entrega são AArch64 de verdade, e o
  sufixo de runtime dos ports (`libs.aarch64`) é o que ela usa;
- **SDL/runtime**: o helper de plataforma do PortMaster existe, a geometria é
  derivada do runtime (nunca cravada) e os padrões de GL4ES são os da imagem;
- **descoberta do controle**: a firmware exporta
  `SDL_GAMECONTROLLERCONFIG_FILE` apontando para o banco que ela mesma
  entrega, e esse banco **não** nomeia o pad deste aparelho.

Esse último ponto é o elo com a fixture: a tabela de botões **não** é
re-derivada aqui. Ela é o caso já validado `trimui-smartpro-crossmix` em
`framework/tests/fixtures/controls/`, cuja conclusão é que a autoridade é o
fallback evdev justamente porque o banco não tem entrada para o pad. Este gate
prova que a firmware oficial continua coerente com aquela conclusão — e falha
se um dia deixar de ser.

## Fronteira

Prova de host: árvore, declarações e ABI. Painel, DRM, áudio, input físico e
comportamento em jogo continuam exigindo prova física separada.
