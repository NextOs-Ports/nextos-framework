# SDL3 privado e mapping PortMaster — contrato V4 opt-in

## Escopo

Este componente corrige uma fronteira específica: um launcher PortMaster pode
entregar `SDL_GAMECONTROLLERCONFIG` no dialeto SDL2/joydev, enquanto uma SDL3
privada numera os mesmos `EV_KEY` em outra ordem. Ele não substitui SDL3, não
descobre uma engine e não transforma evdev em teclado.

A origem comportamental deste slice é o Beach Buggy Racing 1.0.6, commit
`5d965512550c29c2c47a7fb69341fe75349e08fb`, testado fisicamente com controles
funcionais no RG40XX-H/muOS. Colisões e receipts do consumidor são hardening
novo do V4. Staging pré-init e readback efetivo também são correções V4 novas:
o BB1 prova descoberta física, não prova essas duas fronteiras. Todas possuem
provas herméticas no host e prova física ainda obrigatória em cada port opt-in.
O BB2 não é evidência física nem baseline comportamental.

## Ordem obrigatória

1. O launcher torna disponível o mapping não vazio do PortMaster. Na thread dona
   de SDL, e antes de qualquer subsistema SDL, o adapter chama
   `nxinput_sdl3_pm_stage_before_sdl_init()`. A função copia o mapping para
   armazenamento privado limitado e remove `SDL_GAMECONTROLLERCONFIG` do
   ambiente. Isso impede que `SDL_Init` registre o dialeto legado com prioridade
   USER, superior à prioridade API de `SDL_SetGamepadMapping`. Falha de cópia ou
   remoção é terminal antes de `SDL_Init`; integrar depois de `SDL_WasInit(0) != 0`
   falha com `EBUSY` sem tocar no ambiente.
2. O guest continua chamando `SDL_Init` na ordem nativa depois do staging. O
   componente não inicia SDL nem antecipa a enumeração.
3. Imediatamente após `SDL_GetJoysticks`, cada instance ID passa por `prepare`.
   O mesmo boundary é repetido antes de `SDL_IsGamepad` e antes de
   `SDL_OpenGamepad`; somente um mapping lido de volta e verificado pode vir do
   cache.
4. O event node retornado pela própria instância SDL é aberto com
   `O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC`, validado como character device e medido
   com `EVIOCGBIT(EV_KEY)`.
5. Somente a assinatura completa por capacidades converte ordinais. O resultado
   convertido, ou o mapping staged intacto quando a conversão não se aplica, é
   registrado na instância com prioridade API.
6. Depois do setter, `SDL_GetGamepadMappingForID` precisa devolver todas as
   mesmas ligações semânticas de entrada. Nome, ordem dos campos e metadata SDL
   podem variar; semantic/binding divergente, duplicado, ausente ou extra falha
   com `EFFECTIVE_MAPPING_MISMATCH`. O retorno verdadeiro do setter sozinho não
   é prova, pois SDL3 também o devolve quando um mapping USER impede a troca.
7. A preparação é best-effort como no BB1 aprovado. O adapter sempre preserva
   a chamada nativa de classificação/Open; uma falha fica no receipt e é
   tentada novamente, sem transformar falta de acesso evdev em pad morto.

## Cache, hotplug e colisões

Quando existe mapping, o cache é limitado a 16 instâncias compatíveis com essa
mesma entrada. Ausência de mapping é passthrough e não ocupa cache. Apenas o
mapping convertido ou staged intacto cujo readback semântico passou pode ser
cacheado, sempre com `effective_mapping_verified=1`. Falhas de path, open,
fstat, tipo, ioctl, conversão, registro ou readback são transitórias e devem ser
tentadas novamente na próxima fronteira.

O adapter deve encaminhar eventos de remoção à API `remove`; o componente não
drena a fila SDL automaticamente. Remoção, enumeração, classificação, Open e
receipt são chamadas serializadas na mesma thread dona do loop SDL. Sob esse
contrato, a remoção apaga apenas a instância correspondente e uma nova conexão
é preparada como nova identidade. Embora a medição comece por instance ID, a
SDL3 armazena o mapping por GUID; duas instâncias que exigem mappings divergentes
com o mesmo GUID não são isoladas. O coordenador arbitra tanto rewrites quanto
mappings staged intactos, registra a colisão e falha fechado em vez de escolher
uma delas silenciosamente.

## Receipt do consumidor real

Classificação, Open, eventos observados, parser e dispatcher são evidências
intermediárias. O receipt de gameplay só avança quando o adapter chama a API de
confirmação no wrapper/callback que realmente devolveu o press/release à engine.
O adapter registra sem paths, nomes de device, VID/PID ou mapping completo.

## Opt-in e adoção

Esta entrega fecha no host as fronteiras de prioridade USER/API, readback
efetivo, cache e receipt, mas não fornece uma SDL3. A descoberta que distinguiu o
BB1 1.0.6 continua um gate obrigatório da SDL privada do port: backend Linux
evdev habilitado, udev carregada dinamicamente e nenhuma dependência
`DT_NEEDED` em `libudev`. O contrato e suas mutações negativas são verificados
por `tests/test_sdl3_discovery_contract.py`.

## Onde o opt-in NÃO entrou, e por quê

A integração declarativa foi feita **onde realmente é necessária** e em
lugar nenhum além disso:

- **`nxgenerator`** ganha `controls.sdl3_portmaster` (desligado por omissão) e
  **`nxrelease`** valida o bloco: são as duas camadas que decidem o que entra
  no pacote e precisam saber que a SDL3 privada está pinada.
- **`nxbootstrap` não recebeu nada.** A staging acontece antes de `SDL_Init`
  **dentro do processo do jogo**, não no launcher. Dar ao launcher um campo que
  ele não lê seria contrato morto: mais superfície para divergir, zero
  comportamento.
- **`nxobs` não recebeu nada.** O receipt do consumidor é emitido pelo adapter,
  que já é o único ponto capaz de provar que a engine observou o estado.
  Duplicar um schema no nxobs não acrescentaria prova nenhuma.

Isso é decisão registrada, não omissão: se um dia o launcher ou o nxobs
precisarem realmente do campo, ele nasce com o motivo escrito junto.

## Listas heterogêneas (`nxinput_sdl3_pm_select_mapping`)

Um `SDL_GAMECONTROLLERCONFIG` real é uma lista separada por `\n`: o PortMaster
e as CFWs entregam uma entrada por device conhecido, em dialetos diferentes,
com linhas em branco e comentários `#`. Nada garante que a entrada deste
aparelho seja a primeira.

A seleção acontece **antes** de qualquer conversão e é deliberadamente
conservadora, porque instalar a entrada errada é pior do que não instalar
nenhuma:

| Situação | Resultado |
|---|---|
| exatamente uma entrada | usada, exatamente como antes |
| várias entradas, uma com o GUID deste device | essa entrada é usada, esteja onde estiver |
| várias entradas, nenhuma deste device | **passthrough**: nenhuma chamada SDL, nenhum cache, fluxo do guest intacto |
| mesmo GUID duas vezes, bytes idênticos | aceito |
| mesmo GUID duas vezes, bytes divergentes | falha fechada (`EPROTO`) — a store da SDL3 é indexada por GUID e a ordem nunca pode decidir |
| entrada malformada | falha fechada, nunca ignorada — uma linha truncada pode ser justamente a deste device |

O receipt ganhou `staged_entry_count` com a quantidade real de entradas.

O gate também aceita `--adopter-receipt <json>`. O receipt usa schema
`nxinput-v4-sdl3-adoption-receipt/1`, inclui o objeto `private_sdl3` com os três
campos de descoberta acima e `discovery_before_classification: true`. Ele deve
ser produzido pela auditoria do ELF/runtime do port; copiar o receipt do BB1 não
substitui essa prova.

O alvo fica desligado por padrão. Cada port adota por branch própria, bump de
versão e ZIP novo; nenhum port V3 é migrado automaticamente. O port deve fixar
uma SDL3 privada que satisfaça integralmente o gate de descoberta acima, provar
que a descoberta ocorre antes da primeira classificação, testar hotplug e
múltiplas instâncias compatíveis conforme seu escopo e obter prova física do
mesmo artefato antes de declarar suporte. O receipt positivo do BB1 prova a
descoberta daquela SDL; ele não é prova física herdável do readback novo nem do
consumer sink de outro port.
