# Ambiente ArkOS/dArkOSRE no PC

Reproduz no PC o userspace **multiarch** dessas firmwares a partir da imagem
oficial local. A aceitação física já registrada do port publicado é **citada,
nunca repetida**.

A imagem nunca entra no Git e nunca é redistribuída. A leitura é somente
leitura (`debugfs` com deslocamento da partição): sem montar, sem loop device,
sem root e sem escrever um byte no arquivo de origem.

## Preparar e verificar

```sh
python3 framework/tests/device-environments/arkos/prepare.py \
  --image /caminho/para/arkos-...img \
  --output /caminho/para/firmware-environments/arkos-r36s-v2.0

python3 framework/tests/device-environments/arkos/verify.py \
  --environment /caminho/para/firmware-environments/arkos-r36s-v2.0
```

## O que fica provado

- **raízes multiarch**: `/usr/lib/aarch64-linux-gnu` e
  `/usr/lib/arm-linux-gnueabihf`, cada uma julgada pelos ELFs de runtime que
  expõe, com o interpretador da própria ABI;
- **script de linker**: `libc.so` do Debian multiarch existe **na imagem
  oficial**, ao lado da `libc.so.6` de runtime, e é reconhecido como script —
  a regra é ignorá-lo sem recusar a raiz que o contém. Nenhum arquivo não-ELF
  fica sem classificação nas raízes;
- **cadeias de soname** de EGL, GLES, GBM e DRM, cada uma resolvida até um ELF
  da ABI certa, com o `DT_SONAME` conferido contra o nome pelo qual foi
  encontrada;
- **KMSDRM**: a SDL da própria imagem, consultada sob qemu, compila KMSDRM nas
  duas ABIs;
- **rota ARMHF do perfil dArkOS**: os perfis `arkos` e `darkosre` são conferidos
  contra este layout. Foi assim que se descobriu que o perfil `darkosre`
  declarava apenas AArch64 enquanto a firmware entrega multiarch — corrigido
  com a rota `armv7`, sem repetir prova física.

## Fronteira

Prova de host: ABI, isolamento, classificação de arquivo, soname e driver
compilado. Painel, DRM real, compositor, áudio e input continuam exigindo prova
física separada.
