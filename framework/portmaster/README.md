# Contrato PortMaster do framework universal

Este diretório fixa a fronteira entre um launcher pequeno e a integração real do
PortMaster. O princípio é **detectar primeiro, corrigir depois e nunca forçar algo
por padrão**.

O contrato não transforma um port comprovado num device em evidência universal. Ele
padroniza descoberta, ordem de inicialização, controles, ABI, instalação, cleanup e
segurança. Vídeo, áudio e quirks continuam dependentes de capacidades medidas e dos
gates físicos de cada combinação.

A versão 2.1.1 aceita na geração V2 o papel `runtime-data` do nxbootstrap
0.7.2. Ele representa dados imutáveis de runtime gerenciado, sempre `0644` e
autenticados por path/SHA-256; não é reinterpretado como ELF nem como owner data.

## Fontes reproduzíveis

[`upstream-sources-v1.json`](upstream-sources-v1.json) fixa commits e SHA-256 de:

- PortMaster-GUI `8f9ddc4b0f75dfe61eb370bd3d1b4ec9d5ef6967`, usado para runtime,
  `control.txt`, `funcs.txt`, mods e HarbourMaster;
- PortMaster-New `df9805908db41b32c18532e78b9844ebe8c0a768`, usado para a política
  atual de submissão e runtimes;
- snapshots oficiais de `funcs.txt` v1, v2 e v3;
- integração NextOS `2026.07.05-0600`, exclusivamente como evidência local e como
  fonte negativa do hook global legado.

Nenhum script de jogo é fixture do marco M03. WIP é proibido como fonte positiva.
Pikmin, PartyBoard, GTA, Bully, Dysmantle, LIMBO e Chrono Trigger permanecem
referências limitadas ao que foi realmente provado; não demonstram compatibilidade
universal.

As regras de integração ficam em [`contract-v1.json`](contract-v1.json), e os dez
cenários reproduzíveis em
[`fixtures/contract-cases-v1.json`](fixtures/contract-cases-v1.json). O contrato
aditivo [`contract-v2.json`](contract-v2.json) fixa o parser real, o schema
suportado e o ciclo offline de instalação, descoberta, atualização, desinstalação
e reinstalação descrito abaixo. O sucessor
[`contract-v3.json`](contract-v3.json) cobre também o PortMaster oficial legado
`2024.03.10-0841` para a representação de uma lista de runtimes vazia.

No update, uma geração anterior só é preservada quando `GENERATION.json` e a
closure inteira do store se autenticam mutuamente. Receipt ou store órfão é
estado split inválido até em instalação limpa; somente um ZIP realmente sem os
dois pode adotar o generation store pela primeira vez.

## Ordem obrigatória

O fluxo anterior ao jogo é:

1. declarar ABI e o literal `PORT_32BIT="Y"` no wrapper ARMHF;
2. encontrar um `control.txt` regular e carregá-lo;
3. aceitar a `controlfolder` canônica publicada pelo próprio controle;
4. carregar `mod_${CFW_NAME}.txt` somente com nome sanitizado e arquivo regular,
   não-symlink;
5. chamar `get_controls` se existir, sem inventar GUID, VID/PID ou layout;
6. validar ELF, arquitetura e linker dinâmico;
7. preparar paths do host, chamar `pm_platform_helper` no máximo uma vez com o
   executável real e, se `PM_PIPE` continuar ativo, fechar o FIFO comprovado por
   `PortMasterDialogExit`;
8. executar NXExtract e o prepare declarado em primeiro plano;
9. adicionar dependências privadas declaradas, iniciar um filho direto e aguardá-lo;
10. chamar `pm_finish` exatamente uma vez e devolver o status do jogo.

`pm_platform_helper` é uma dica de plataforma, não um launcher. Falha dele é
registrada e o fluxo continua. Falha de `pm_finish` também é registrada, sem
substituir o status real do jogo.

## Descoberta das raízes

`nxbootstrap` tenta primeiro uma `controlfolder` válida já publicada. Depois procura
raízes por capacidade:

| Família observada | Raízes candidatas relevantes |
| --- | --- |
| ArkOS/genérica | `/roms/tools/PortMaster`, `/roms2/tools/PortMaster` |
| ROCKNIX/JELOS | `/opt/system/Tools/PortMaster`, `/storage/.config/PortMaster` |
| muOS | `/mnt/mmc/MUOS/PortMaster` e raízes publicadas pelo controle |
| Knulli/Batocera | `XDG_DATA_HOME/PortMaster`, `/userdata/system/.local/share/PortMaster` |
| TrimUI | raízes próprias sob `/mnt/SDCARD` ou `/mnt/sdcard` |
| NextOS | `/storage/roms/ports/PortMaster` |
| MIYOO_EX | candidato de descoberta somente; sem fixture/runtime, `unsupported/unverified` |
| RetroDECK | candidato de descoberta na raiz de dados do Flatpak; sem fixture/runtime, `unsupported/unverified` |

Esses nomes servem para descoberta e log. Eles não selecionam Mesa, EGL, renderer,
backend SDL, áudio ou uma correção de engine. Uma raiz candidata reconhecida não
é evidência de suporte físico.

## Compatibilidade das APIs

`control.txt` não expõe um número de versão estável. `PM_FUNCS_VERSION` existe, mas
o framework testa cada função antes de usá-la:

| `funcs.txt` | Base observada | Não garantido |
| --- | --- | --- |
| v1 | `bind_directories`, `pm_finish` | `bind_files`, `pm_message`, `pm_platform_helper` |
| v2 | v1 + `pm_message`, cleanup GPTOKEYB | `bind_files`, `pm_platform_helper` |
| v3 | `bind_directories`, `bind_files`, `pm_message`, `pm_platform_helper`, `pm_finish` | — |

O mapping gerado pelo `get_controls` ativo é a autoridade. Mapping vazio ou falha
vira diagnóstico; o shim preserva o ambiente do firmware e não cria um banco de
controles por device.

A integração PortMaster também pode publicar `ANALOGSTICKS` ou o alias
`ANALOG_STICKS` para descrever o número de sticks do controle integrado. O
launcher aceita somente `0`, `1` ou `2` e exporta a cópia sanitizada como
`NXINPUT_ANALOG_STICKS_HINT`; valor ausente ou inválido fica unset. Essa dica é
host-wide e diagnóstica: não identifica pads USB/hotplug, não substitui bindings
SDL por controle e nunca habilita automaticamente D-pad→stick.

## ABI, bibliotecas e filesystem

`PORT_32BIT=Y` declara ao scanner/mod que o port já é ARMHF. Não converte um ELF nem
prova o linker `/lib/ld-linux-armhf.so.3`. AArch64 exige
`/lib/ld-linux-aarch64.so.1`; os targets usam fonte/API comum e artefatos separados.

Na fase do jogo, a ordem é: paths privados declarados, bibliotecas do PortMaster,
paths da ABI do firmware, paths herdados que não pertencem ao port e paths genéricos.
Na fase de setup não entram bibliotecas privadas. Provedores privados de EGL, GL,
GLES, GBM, DRM, Mali ou Mesa são rejeitados.

FAT/exFAT não é tratado como POSIX. Helpers vendorizados são chamados por `bash`, o
lock fica no runtime/tmp privado e um bind só é usado se o port precisar dele e a
função existir. Não se pressupõe `chmod` persistente, symlink ou `flock` nos dados.

## Instalação e saída

HarbourMaster atualiza metadados antigos, mas o formato atual fixado é `port.json`
v4. Um item terminado em `/` representa diretório. O launcher `.sh` de topo vai
para `scripts_dir`; os demais membros vão para `ports_dir`. Caminhos absolutos e
travessia por `..` são rejeitados, e permissões são reparadas após a extração.

O shim não administra frontend. Não chama `systemctl`, `loginctl`, `setsid`, logout,
bloqueio, suspensão, reboot ou poweroff. Sinais alcançam somente o PID filho direto,
confirmado por PID e starttime. Ao terminar, o launcher deixa o supervisor existente
cuidar do frontend.

## Evidência negativa NextOS

O hook local legado `portmaster_compatibility.sh` reescreve todos os launchers,
injeta providers SDL por device e remove bibliotecas de diretórios de ports. Ele está
registrado como **negative-only**, com reutilização proibida. O substituto correto é
um ajuste process-local, aplicado somente ao port atual depois de uma falha medida.

O launcher local do PortMaster também pode reiniciar `emustation.service`; isso não
é responsabilidade do framework e não será chamado durante os gates do shim.

## Gates estático e real offline

O primeiro teste não cria processos de jogo, não acessa aparelhos e não altera o
host:

```sh
python3 framework/portmaster/tests/test_portmaster_contract.py
```

Ele valida fontes e hashes, fixtures, APIs v1–v3, ordem do bootstrap, roots,
instalação v4, ARMHF, FAT/exFAT, ausência de overrides de vídeo/áudio e ausência de
comandos de processo, sessão, serviço ou energia.

O segundo gate carrega o HarbourMaster oficial e não modificado do commit fixado
`8f9ddc4b0f75dfe61eb370bd3d1b4ec9d5ef6967`:

```sh
python3 -B framework/portmaster/tests/test_harbourmaster_cycle.py
python3 -B framework/tests/audit-portmaster-zip.py \
  /caminho/port-novo.zip --previous /caminho/port-anterior.zip
```

O snapshot vendorizado tem inventário e SHA-256 próprios e inclui as licenças do
upstream e das dependências Python necessárias. Cada execução bloqueia sockets,
usa uma raiz temporária exclusiva e nunca executa launcher, ELF ou dado do jogo.
Além do JSON estrito e do schema suportado, o gate chama `port_info_load`,
`HarbourMaster.load_ports`, instala e redescobre o port, aplica uma atualização,
desinstala e reinstala. Membros obsoletos continuam fatais. A única exceção não é
um membro stale: é a geração anterior completa sob
`.nxruntime/generations/<id>/`. Uma v2 fica disponível como rollback A/B; uma v1
histórica permanece control-only, inerte e não selecionável até uma futura GC
state-aware, exatamente como o launcher documenta.
Ela só passa quando o `GENERATION.json` do ZIP anterior autentica a closure exata,
ordenada, com path, modo e SHA-256; o manifesto e o commit concordam com o id e
todos os bytes reaparecem intactos. O mesmo id exige closure integralmente
idêntica; arquivo solto, root extra, closure parcial ou byte divergente continua
falhando.
O primeiro update de um ZIP antigo sem store para um candidato autenticado é
uma adoção, não um rollback. Para recibos nxgenerator até 0.2.18, a ordem
autenticada continua sendo a ordem histórica por componentes de path; 0.2.19+
usa a ordem POSIX textual. O `execution_roles` opcional do v1 mixed-ABI precisa
ser idêntico ao `nxport.json` guardado.
O fixture negativo v1 continua provando a falha do caminho de migração antigo
quando `attr.runtime` falta. Para metadado v4, porém, os dois parsers oficiais
divergem: o HarbourMaster atual aceita o campo ausente e o normaliza para `[]`,
enquanto `2024.03.10-0841` o normaliza para `None` e pula a instalação de runtime.
Esse legado quebra se receber `[]`, pois tenta formatar a lista como string.
Portanto, quando a declaração validada do projeto é uma lista vazia, o gerador
omite `attr.runtime` do `port.json`; a declaração no `nxproject.json` continua
obrigatória. Listas não vazias não recebem aqui uma nova alegação de suporte
legado.

Sem `--previous`, o auditor ainda prova instalação, descoberta, desinstalação e
reinstalação do candidato. Com `--previous`, ele também exige uma atualização
limpa do ZIP anterior para o candidato. Esses resultados são evidência de parser
e filesystem sintético, não de execução do jogo ou suporte físico.
