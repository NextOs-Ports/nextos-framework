# Recuperação, rollback e publicação crash-safe do nxrelease

O nxrelease nunca sobrescreve um destino existente e trata toda publicação como
uma operação que pode ser interrompida a qualquer momento (queda de processo,
falta de espaço, `SIGINT`). Em nenhum caminho um artefato público fica visível
sem seu hash de acompanhamento, e em nenhum caminho um arquivo alheio é
apagado. Este documento descreve o que o gate faz sozinho e como recuperar um
stage/publicação quando algo dá errado.

## Princípios

1. **Destino novo é obrigatório.** `stage`, `--output` (build) e
   `--destination` (bundle) recusam um caminho que já existe, inclusive um
   symlink. Para refazer uma release, use um destino novo. Não há "forçar
   sobrescrever": isso eliminaria a garantia de no-overwrite.
2. **Falhar fechado.** Se o filesystem não puder garantir `renameat2(
   RENAME_NOREPLACE)` (`ENOSYS`/`EINVAL`/`EOPNOTSUPP`) ou hard links, o gate
   aborta em vez de cair num modo menos seguro.
3. **Nada é autoridade além do inode+timestamp/diretório atômico.** Paths são
   diagnóstico; a publicação confia em `rename`/`link` POSIX, não em checar
   "o arquivo sumiu".

## Stage: diretório temporário + rename atômico

`stage_release` constrói o stage num `tempfile.mkdtemp(prefix=".nxrelease-stage-",
dir=parent)` (mesmo filesystem do destino). O destino só aparece por uma única
operação `renameat2(RENAME_NOREPLACE)` (`rename_noreplace`) depois de o stage
passar por `verify_stage`. O `finally` remove o temporário se a publicação não
concluiu. Logo, **ou o destino existe completo, ou não existe nada**: nunca há
um stage parcial visível no caminho final.

Para descartar um stage interrompido que deixou um `.nxrelease-stage-*` órfão no
pai do destino, remova manualmente esse diretório oculto (liste antes):

```sh
ls -la /caminho/para/pai-do-destino/.nxrelease-stage-*
rm -rf /caminho/para/pai-do-destino/.nxrelease-stage-XXXX
```

Não use o caminho final como destino novamente: ele ou não existe (comum) ou, se
existir de uma tentativa anterior bem-sucedida, deve ser preservado.

## Build: dupla ZIP + .sha256 sem overwrite

`create_archive` escreve o ZIP e o `.sha256` em temporários no mesmo diretório
do destino, depois `publish_archive_pair` publica os dois:

- um lock por destino `.<output>.nxrelease-publish.lock` é criado com
  `O_CREAT|O_EXCL` (serializa publicadores nxrelease concorrentes);
- o `.sha256` é linkado primeiro e o ZIP depois — um ZIP público nunca fica
  visível sem seu hash;
- em falha/concorrência, o rollback remove **somente** o inode que o próprio
  nxrelease acabou de criar (`unlink_if_same` compara `(st_dev, st_ino)`), nunca
  um arquivo que pertença a outro dono;
- se o destino apareceu entre a validação e a publicação, o gate falha com
  *“destination appeared concurrently; refusing to overwrite”* e nada é
  sobrescrito.

Como POSIX não oferece transação de duas entradas, há uma janela minúscula em
que o `.sha256` está visível e o ZIP ainda não. Para visibilidade
crash-atômica conjunta, use `bundle`.

## Bundle: transação de diretório

`bundle` monta ZIP + `.sha256` num diretório oculto e faz **uma única**
`RENAME_NOREPLACE` do diretório inteiro para o destino. É o caminho público mais
forte: as duas entradas aparecem juntas ou não aparecem, mesmo diante de queda
do processo. Em falha, o diretório oculto `.nxrelease-bundle-*` é removido no
`finally`. Um bundle interrompido pode deixar um `.nxrelease-bundle-*` órfão no
pai do destino; remova-o manualmente (liste antes), como acima.

## Reauditoria independente

Qualquer stage ou ZIP pode ser reauditado sem rerodar o build:

```sh
python3 framework/nxrelease/nxrelease.py verify-stage --stage /caminho/stage
python3 framework/nxrelease/nxrelease.py verify \
  --archive /caminho/jogo.zip --sha256-file /caminho/jogo.zip.sha256
python3 framework/nxrelease/nxrelease.py verify \
  --archive /caminho/jogo-novo.zip \
  --sha256-file /caminho/jogo-novo.zip.sha256 \
  --previous-archive /caminho/jogo-anterior.zip
```

`verify` reabre o ZIP, confere CRC, ordem, timestamps, modos, inventário,
`MANIFEST.sha256`, o `SBOM.cdx.json` (cobertura + hashes contra o inventário),
NXExtract e todos os ELFs (inclusive ELF sem section headers). Se o
`MANIFEST.sha256` ou o SBOM não baterem com o payload, o ZIP foi adulterado e o
gate falha.
Com `--previous-archive`, a mesma reauditoria executa ainda, em roots
descartáveis e sem rede, o ciclo real do HarbourMaster: instalação anterior,
reinício/discovery, update pelos bytes novos, uninstall restrito e reinstall.
O jogo, o launcher e os ELFs do pacote nunca são executados nesse gate.

## Reprovenância e rollback de uma release publicada

- Cada release é um destino novo; a release anterior permanece intacta no seu
  próprio caminho. O rollback de produção = apontar o consumidor de volta para
  o caminho/SHA da release anterior.
- O `SBOM.cdx.json` e o `MANIFEST.sha256` dentro de `<port>/.nxrelease/`
  provêm a proveniência exata: inventário, hashes, `elf_audit` (com
  `glibc_max`, `interpreter`, `provenance`), pins do nxbootstrap, nxsplash e NXExtract,
  e o `input_manifest_sha256` do manifesto de entrada. Com isso, qualquer ZIP
  pode ser reconstruído/verificado de forma determinística a partir do manifesto
  e das fontes pinadas.
- Nunca republique bytes antigos sobre um caminho novo após mudar código: o
  `input_manifest_sha256` e os pins de SHA-256 dos arquivos detectam qualquer
  divergência fonte↔manifesto entre `validate` e `stage`, e o `verify` final
  confirma os bytes publicados.

## filesystem

`renameat2(RENAME_NOREPLACE)` e hard links exigem um filesystem POSIX
(ext4/xfs/btrfs). FAT/exFAT/NILFS não suportam essas operações e o gate falha
fechado — mantenha `--stage`, `--output` e `--destination` num filesystem Linux
do host, nunca diretamente num cartão removível.
