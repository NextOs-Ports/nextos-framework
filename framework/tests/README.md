# Infraestrutura segura de testes

[`test-matrix-v1.json`](test-matrix-v1.json) classifica cada gate em quatro grupos:

- `pure`: somente leitura/validação estática;
- `filesystem`: escreve apenas numa árvore `mktemp` própria e não envia sinais;
- `process`: testa supervisão/sinais exclusivamente no user/PID/mount namespace
  selado;
- `hardware`: nunca executa automaticamente no host.

Todo gate automático passa por `framework/tools/run-logged.sh`. A ordem canônica é
`test_infrastructure.py` primeiro e `toolchain-preflight` logo depois; se a
infraestrutura, um compiler/sysroot ou a imagem Docker fixada não estiver disponível
com a identidade exata, nenhuma suíte maior é autorizada. O preflight apenas inspeciona
recursos locais e nunca baixa ou substitui uma imagem. Teste físico continua separado
e exige autorização de IP da sessão atual.

O M07 acrescenta um ledger estático de 20 requisitos e um gate de release
NXExtract completo (`--require-ui`): 56 casos sintéticos, runtime isolado,
pinagem integral, membros regulares/não vazios e auditoria de todos os ELFs.

O ARMv7 tem dois gates separados. `nxloader-armv7-cross` compila com GCC e
Clang/LLD no mesmo sysroot antigo, audita ABI/PT_INTERP/glibc e executa apenas
probes host cross-safe em QEMU; ele declara explicitamente zero ELF/initializer
guest. `nxloader-armv7-physical` é manual e contém somente o probe de cache
ARM/Thumb, sem endereço ou comando remoto embutido.

O ledger `nxloader-m09-audit` fecha os 20 requisitos ARMv7, confere as 56 linhas
ABI de KOTOR/TASM2, preserva a separação entre cross e hardware e valida a prova
física sanitizada. O endereço, hostname, credencial e comando remoto nunca entram
no repositório; o gate automático apenas lê o registro já produzido na sessão
autorizada.

`nxgenerator-framework-pin` é um gate `filesystem` separado. Ele prova que os
componentes vêm de commits completos e objetos Git cuja identidade foi recalculada,
que checkout sujo, replace refs, variáveis `GIT_*`, filtros e lazy fetch não mudam
os bytes, e que snapshot/recibo falham fechado diante de adulteração. O fechamento
M19 agora contabiliza M19-001..021; o ledger M20 permanece M20-001..020 e exige o
novo gate entre `nxgenerator-host` e `nxgenerator-m19-closure`.

O fechamento `nxloader-072-closure` fixa a correção cumulativa dos dois
backends: um hook `SKIP` não pode ocultar import strong sem default e deixar um
`JUMP_SLOT` em PLT0. As fixtures sintéticas ARMv7/ELF32 e AArch64/ELF64 provam
falha atômica, retry, provider/default, weak A/zero, `WRITE` e o opt-in
diagnóstico `ALLOW_UNRESOLVED`, sem executar guest nem acessar hardware. Os
gates lifecycle cross continuam compilando o core com C99 estrito e `-Werror`.

O auditor também compara o runner com a matriz: um comando automático não pode aparecer,
sumir ou mudar seus argumentos sem atualizar a classificação. Todos os scripts Bash,
fontes Python e JSON do escopo passam por validação sintática. Tokens de acesso remoto,
energia, sessão, serviço ou varredura ampla de processos só podem existir nos validadores
estáticos revisados; nunca nos executores automáticos.

O runner de processos mantém descritores abertos dos namespaces originais. Assim, uma
variável de ambiente copiada ou falsificada não basta para liberar o teste. Dentro do
namespace, um watchdog registra PID+starttime do único filho e aplica limites de CPU,
memória, arquivo e tempo. A suíte isolada usa 4096 segundos apenas como fusível finito
de parede para tolerar contenção externa; CPU e invariantes internas continuam sendo
os limites efetivos de regressão. TERM/KILL só podem alcançar esse filho e somente
depois de revalidar o starttime. Quando o PID 1 do namespace termina, o kernel remove
qualquer resto interno; não existe fallback para o host.

`test-tools.sh` prova status/log/manifesto, redação de credenciais conhecidas,
checkpoint append-only e restauração byte a byte de tracked, untracked e symlink.

Execução local canônica:

```sh
bash framework/tests/run-safe-gates.sh \
  --log-root /caminho/absoluto/para/logs
```

`chrono-m21-host`, `chrono-m23-audit` e `chrono-m24-closure` não fazem parte
desse runner. Eles recompilam ou exigem o ZIP congelado de uma release antiga
do Chrono, portanto são gates manuais específicos daquele port
(`automatic=false`, `scope=legacy-port-specific`). Só devem ser executados
numa sessão dedicada do Chrono; uma divergência de hash não pode ser aceita nem
recarimbada sem a validação física do novo binário. Os audits process-free M21
e M22, que não dependem desse artefato local, permanecem automáticos.

O resumo final precisa declarar `hardware_ran=0 device_access=0` e
`guest_initializers_executed=0`. Integração física não tem comando na matriz automática;
ela só pode ser criada numa etapa de device explicitamente autorizada e com log separado.

## Prova física em firmware com ambiente de frontend — regra obrigatória

Registrado em 22/08/2026, depois de um dia inteiro de provas por SSH
aprovarem dois ports que ficavam em TELA PRETA quando abertos pelo menu: em
firmwares cuja unidade de frontend declara `Environment=` (dArkOSRE exporta
`SDL_VIDEO_EGL_DRIVER=libEGL.so`; ArkOS idem — nunca confundir os dois nos
registros), **uma prova física só vale lançada com o ambiente do frontend
reproduzido** — `framework/nxobs/nx-device-launch.sh` lê o unit no aparelho e
relança sob `env -i` com exatamente aquelas variáveis, coleta o veredito do
frame proof e a imagem de prova (`NXLAUNCH_PROOF_DIR`). Shell SSH puro não
herda o unit e exercita OUTRO caminho gráfico; um PASS por SSH não autoriza
release nesses firmwares. O fato do hint fica pinado por perfil em
`firmware-profiles-v2.json` (`frontend_environment`), e a classe de falha
"hint desprovado por renderer morto" tem gate hermético no
`nxgl-provider-discovery` (cenário 14) e no recibo de vídeo (`dead-context`).

## Simulação de contrato de firmware

[`firmware-profiles-v1.json`](firmware-profiles-v1.json) descreve seis fixtures
de ambiente — ArkOS, muOS, ROCKNIX, dArkOS, NextOS e Knulli/Batocera — para
exercitar somente as fronteiras compartilhadas do launcher e do pacote. O mesmo
arquivo liga esses ambientes às seis famílias sanitizadas do catálogo de devices
e a uma observação dArkOS, sem multiplicar CFW × placa × ABI. Um resultado
`profile-contract-pass` significa que arquivos e processos sintéticos obedeceram
ao contrato declarado. Ele nunca significa que um CFW, aparelho, GPU, saída de
áudio ou controle foi validado.

O gate puro [`test_firmware_profiles.py`](test_firmware_profiles.py) impede que
um perfil vire evidência universal, confere roots/casos contra o contrato
PortMaster e garante que os gates fake de nxcompat, nxgl e nxaudio sejam
reutilizados uma única vez na matriz. O gate de processo
`nxbootstrap/tests/test-firmware-launchers.sh` fica dentro do mesmo namespace
selado do nxbootstrap e mede, para os seis perfis, split root, descoberta de
`control.txt`, mapping, `PORT_32BIT`, precedência de libs, ausência de `stat`,
status e finalização. Ele não abre dispositivo.

Para um ZIP já construído, o preflight genérico é:

```sh
python3 -B framework/tests/audit-portmaster-zip.py \
  /caminho/port-novo.zip --previous /caminho/port-anterior.zip
```

Ele rejeita paths/tipos perigosos, exige um launcher raiz, aplica `bash -n` ou
`sh -n` a todos os shells reconhecidos, bloqueia chamada ativa ao comando
externo `stat`, impede que nomes de sites/origens dos dados do dono apareçam em
paths ou textos públicos (`.md`, `.txt`, `.json`) e delega todos os ELFs
encontrados ao `nxabi` público. Em seguida, o HarbourMaster oficial fixado é
carregado sem modificações e com rede bloqueada numa raiz temporária exclusiva.
O gate prova parser, instalação, descoberta, desinstalação e reinstalação; quando
`--previous` é informado, também prova atualização e rejeita qualquer arquivo
obsoleto deixado pela versão anterior. Launcher, ELF e dados do jogo nunca são
executados. Sem `--previous`, somente o ciclo do candidato é exigido. Uma release
com `nxrelease.json` continua tendo que passar pelo NXRelease completo; este
preflight não substitui pins, proveniência, licenças, SBOM nem o re-open do ZIP.

### Matriz v2 por firmware e ABI

A matriz v1 continua intacta. A extensão
[`firmware-profiles-v2.json`](firmware-profiles-v2.json) separa muOS,
ROCKNIX/Panfrost, AmberELEC, Knulli/Batocera, ArkOS, dArkOSRE,
NextOS/Mali-450, TrimUI e spruceOS/Miyoo Flip, em vez de agrupar famílias sem
a mesma evidência.

O gate puro [`test_firmware_matrix_v2.py`](test_firmware_matrix_v2.py) valida
fontes aprovadas, roots/control paths, detecção segura de CFW, rotas
AArch64/ARMv7 e identidade ELF da UI NXExtract/NXSplash. O gate de processo
[`run-firmware-matrix-v2.sh`](run-firmware-matrix-v2.sh) roda dentro do mesmo
namespace selado do bootstrap, com root sintético isolado e rede desabilitada.
Cada caso prova launcher sem `stat`, status real, `pm_finish` único e log precoce
`0600`. O contrato completo e seus limites estão em
[`FIRMWARE-MATRIX-V2.md`](FIRMWARE-MATRIX-V2.md).

O recibo segue
[`firmware-matrix-receipt-v2.schema.json`](firmware-matrix-receipt-v2.schema.json).
O resultado automático é sempre `evidence.kind=synthetic`,
`hardware_ran=false` e `device_access=false`; a variante `physical` exige outra
sessão, autorização explícita e SHA-256 do artefato final.

### Ambientes locais por device

[`device-environments/`](device-environments/) contém contratos e preparadores
opt-in para reproduzir no PC os userspaces reais sem versionar firmware. O
primeiro ambiente é o
[`spruce/Miyoo Flip`](device-environments/spruce/README.md): ele valida host
AArch64, NXExtract AArch64, NXSplash/jogo ARMHF, loader alternativo, chroot
ARMHF, separação entre usr/lib32 e usr/lib e a SDL real KMSDRM/dummy.

Esse teste é executado somente quando o arquivo oficial é fornecido localmente.
Ele usa QEMU apenas para consultar a SDL ARMHF sem inicializar vídeo ou acessar
hardware.

O segundo ambiente é o [`muos/RG40XX-H`](device-environments/muos/README.md):
ele lê a imagem oficial com `debugfs` somente leitura, sem montar nem usar
loop device, e valida as duas raízes reais (AArch64 e ARMHF), o isolamento
entre elas, o interpretador Python que a imagem entrega, os provedores de
renderer de cada ABI e os três casos de hint SDL — sem hint, compatível e
incompatível — contra a lista de drivers que as próprias SDLs da imagem
compilaram. Controles do H700 não são repetidos ali: já têm gate próprio.

A fixture [`knulli`](device-environments/knulli/README.md) cobre **somente
viewport**: lê da imagem oficial o que a firmware declara sobre o painel
(placa, autoridade de resolução, geometrias internas, modos HDMI e a rotação
ofertada comentada) e submete esses números ao seletor puro do nxgl — painel
quadrado e painel retrato entram literalmente, e sem fonte alguma o resultado é
erro explícito. Ordem de botões, mapeamento e chord não são duplicados ali.

O ambiente [`arkos`](device-environments/arkos/README.md) cobre as duas raízes
multiarch dessas firmwares, o script de linker `libc.so` que o Debian entrega
ao lado da libc de runtime, as cadeias de soname de EGL/GLES/GBM/DRM e o
KMSDRM que a SDL da própria imagem compila — e confere a rota ARMHF declarada
pelos perfis ArkOS e dArkOSRE contra esse layout.

O ambiente [`crossmix`](device-environments/crossmix/README.md) parte de uma
distribuição em ZIP (árvore de cartão, não imagem de disco) e cobre as raízes
declaradas pela firmware, a ABI das bibliotecas entregues, as declarações de
SDL/runtime e a cadeia de descoberta do controle — reusando, sem re-derivar, o
caso de controles já validado daquele aparelho.

O ambiente [`amberelec`](device-environments/amberelec/README.md) cobre a
família *ELEC: a biblioteca OpenAL que a firmware entrega junto de um
`alsoft.conf` que força um driver, quais back-ends ALSA/Pulse existem de fato,
o fragmento de login que altera `LD_LIBRARY_PATH` antes do port e a regra de
que o back-end sai de capability declarada pelo port, nunca de nome de
aparelho.

O ambiente
[`ROCKNIX RK3566-Specific`](device-environments/rocknix/README.md) fixa a
identidade da imagem oficial 20260801, a GPT, a partição FAT32 e o `SYSTEM`
SquashFS, além de inventariar o userspace AArch64 de SDL2, Wayland, Sway,
Mesa/Panfrost, EGL e GLES. A configuração Wayland encontrada na própria imagem
é registrada como fato da firmware; ela não vira selector do framework por
nome de CFW, aparelho ou GPU.

Esse ambiente prova somente bytes, estrutura de disco, identidade ELF,
dependências e capacidades que podem ser consultadas sem abrir vídeo. O gate
consulta a SDL alvo e resolve símbolos EGL/GLES por `dlopen`, limpa variáveis
gráficas do host e prova os fallbacks Wayland/provider sobre o código real do
nxgl. Ele não prova backend selecionado, compositor ativo, `panfrost` ligado a
uma GPU, `eglInitialize`, versão GLES, renderer, drawable ou frame físico.
Esses fatos continuam exigindo execução autorizada no aparelho e recibo
separado.

Todo ambiente novo segue o mesmo molde, sem copiar decisão específica de outro
aparelho: contrato versionado com nome, versão, tamanho e SHA-256 do arquivo
oficial; preparação transacional **fora** do repositório; recibo de preparação
com proveniência; e verificação que declara `hardware_ran=0 device_access=0`.
Imagem de firmware nunca entra no Git nem é redistribuída, e fixture não
substitui prova física — nem prova física dispensa fixture reproduzível.

### Smoke rootfs opcional e local

Rootfs/firmware não entra nos safe gates e não é armazenado pelo framework. Um
smoke local deve exigir um manifesto externo com origem, versão, SHA-256,
inventário de licenças e permissão de redistribuição; trabalhar sobre extração
somente leitura e invocar QEMU user explicitamente. Mesmo verde, ele prova no
máximo linker/libc/`dlopen` do userspace: kernel, DRM/Mali, compositor, ALSA,
PipeWire, evdev e controle continuam exigindo prova física separada. ROMs, APKs,
BIOS, saves, dados pessoais e imagens sem proveniência nunca são fixtures.

## Fechamento M20 e checkpoint verde

[`m20-closure-v1.json`](m20-closure-v1.json) liga M20-001..020 aos gates de sintaxe,
sanitizers/fuzz, componentes puros, fixtures sintéticas, NXExtract, NXRelease,
symlink/hardlink/TOCTOU, concorrência, semântica FAT/exFAT simulada, baixa glibc,
reprodutibilidade e logs duráveis. O gate é:

```sh
python3 -B framework/tests/test_m20_closure.py
```

O último comando do runner canônico é
[`capture-green-checkpoint.sh`](capture-green-checkpoint.sh). Como o runner usa
`set -e`, qualquer gate vermelho encerra o conjunto antes desse comando. Só um conjunto
integralmente verde cria o snapshot append-only de `framework/`,
`suportando_outros_devices/` e `publicando_ports/` fora do repositório. Isso não autoriza
hardware: todas as entradas `hardware` continuam manuais, sem comando e sem endereço.
