# Ambiente muOS (RG40XX-H) no PC

Reproduz no PC o userspace real do muOS a partir da **imagem oficial**, sem
versionar firmware e sem tocar em aparelho. A estrutura segue o mesmo molde do
ambiente [`spruce`](../spruce/README.md) — contrato versionado, preparação
transacional fora do repositório, recibo de proveniência —, mas **nenhuma
decisão do outro aparelho foi copiada**: raízes, ABIs, interpretador e
provedores de renderer saem da leitura desta imagem.

O que este ambiente cobre:

- as duas raízes reais: **AArch64** (`/lib`, `/lib64`, `/usr/lib`) e **ARMHF**
  (`/lib32`, `/usr/lib32`), cada uma com o próprio carregador;
- **isolamento 32/64**: nenhum ELF de uma ABI dentro da raiz da outra;
- o **interpretador Python** que a imagem entrega, com a versão medida;
- os **provedores de renderer** presentes em cada ABI;
- os **três casos de hint SDL** — sem hint, hint compatível e hint
  incompatível — contra a lista de drivers que a própria SDL da imagem
  compilou, medida para as duas ABIs.

Controles do H700 **não** entram aqui: já têm gate próprio em
`framework/tests/fixtures/controls/`, e repetir a mesma prova não acrescenta
informação.

## Preparar

A imagem nunca entra no Git e nunca é redistribuída. A leitura usa `debugfs`
somente leitura com deslocamento em bytes da partição `rootfs`: sem montar,
sem loop device, sem root e sem escrever um byte no arquivo de origem. O
preparador confere tamanho e SHA-256 da imagem contra o contrato, confere o
deslocamento contra a tabela GPT real e resolve o fechamento `DT_NEEDED` de
cada semente dentro das raízes daquela ABI.

```sh
python3 framework/tests/device-environments/muos/prepare.py \
  --image /caminho/para/MustardOS_RG40XX-H_2601.1_...img \
  --output /caminho/para/firmware-environments/muos-2601.1
```

A saída é criada de forma transacional **fora do repositório** e traz
`environment-receipt.json` com origem, deslocamento, inventário, tamanho e
SHA-256 de cada ELF, fechamento `DT_NEEDED` e a declaração
`hardware_ran=0 device_access=0`. Dependência não resolvida aborta a preparação.

## Verificar

```sh
python3 framework/tests/device-environments/muos/verify.py \
  --environment /caminho/para/firmware-environments/muos-2601.1
```

Confere proveniência, cada hash do recibo, ABIs, isolamento, interpretador
Python executado sob QEMU, provedores de renderer e — com `clang` e
`qemu-*-static` — pergunta às **duas** SDLs da imagem quais drivers de vídeo
elas compilaram. Os subprocessos não herdam variáveis de loader, QEMU ou
display. Não inicializa vídeo e não acessa DRM, Mali, framebuffer, áudio ou
input.

## Casos de hint SDL

```sh
python3 framework/tests/device-environments/muos/sdl-hint-cases.py \
  --environment /caminho/para/firmware-environments/muos-2601.1
```

Os três casos rodam para cada ABI sobre a lista medida: sem hint mantém a
autodetecção, um driver que a SDL compilou é preservado e um hint que ela não
tem é removido. A decisão é do sanitizador real do nxgl
(`nxgl_sanitize_sdl_video_hint_v2`), compilado com o seam de teste apenas para
receber a lista medida.

## Fronteira

O que este ambiente prova é de **host**: ABI, isolamento de raízes,
interpretador, presença de provedor e driver compilado. Kernel, DRM/Mali,
compositor, áudio e input continuam exigindo prova física separada — e prova
física não dispensa esta fixture reproduzível.

Sobre o Python: aqui fica registrado o que a imagem oficial **entrega**. A
falha de campo que importa no muOS — um `python3` de firmware que morre com
`bad marshal data` na própria stdlib — é defeito por instalação, não
propriedade do binário distribuído, e continua coberta pelo perfil sintético
`muos` mais o gate `python-probe`.
