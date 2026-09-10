# Framework universal PortMaster — fechamento da linha v1

Data do congelamento: **22/08/2026**. Commit de referência: o apontado pela
tag imutável `framework-v1`.

## O que a linha v1 é

A arquitetura de três camadas (launcher único → nxbootstrap → loader com
nxloader+nxcompat+nxgl+nxinput+nxandroid → adapter estreito da engine),
com NXExtract e NXSplash como processos separados, contratos congelados em
`contracts/`, evidência em `catalog/` e a bateria automática de
`tests/run-safe-gates.sh`.

Componentes no congelamento:

| Componente | Versão | Tag |
|---|---|---|
| nxbootstrap | 0.6.29 | `nxbootstrap-v0.6.29` |
| NXExtract (engine) | 1.2.17 canônica; 1.2.14–1.2.16 supported no registry | `nxextract-v1.2.17` |
| NXExtract (UI) | 1.2.16 (pixels da linha 1.2.9, goldens imutáveis) | — |
| NXSplash | 0.1.2 | `nxsplash-v0.1.2` |
| nxgenerator | 0.2.12 | — |
| nxrelease | 0.2.29 | — |
| nxgl | 0.2.12 | `nxgl-v0.2.12` |
| nxinput | 0.4.4 | `nxinput-v0.4.4` |
| nxobs | 0.2.2 | `nxobs-v0.2.2` |
| tests | 1.1.3+ | `framework-tests-v1.1.3` |

## A prova do fechamento

Primeira bateria sequencial oficial completa **ALL PASS count=76**
(`hardware_ran=0 device_access=0`, checkpoint verde capturado) em worktree
limpa do master, em 22/08/2026 — depois de curados os dois vermelhos
crônicos (inventário do catalog e o passo report-only do nxabi, que agora é
honesto em árvore parcial e byte-exato em árvore completa). Provas físicas
do mesmo dia: NXExtract 1.2.16 no dArkOSRE (reciclagem de sessão em voo),
nxobs 0.2.2 em três classes de RAM, ff4 1.0.1/ff4a 1.0.2 no dArkOSRE.

## Regras de governança a partir do v1

1. **`master` é a linha estável travada**: entra somente trabalho que passou
   a bateria completa com diff de falhas vazio e a prova física quando o
   caminho tocado a exige. Push direto é bloqueado; o merge vem de branch.
2. **`release/v1` congela esta linha**: hotfix aprovado entra nela por
   cherry-pick com os mesmos gates, e vira tag própria.
3. **Toda onda nova de desenvolvimento** nasce em branch própria
   (`v2`, `v3`, …) e só encontra o master no fechamento da onda, inteira e
   verde.
4. **Prova física em firmware com frontend-env** (dArkOSRE/ArkOS): só vale
   lançada com o ambiente do unit reproduzido (`nx-device-launch`), com
   veredito de frame proof e imagem de prova. SSH puro não aprova release
   nesses firmwares — lição de 22/08 (tela preta pega pelo teste de olho).
5. As regras existentes continuam por cima: aditividade, opt-in por port,
   interface visual imutável sem autorização, capacidade > nome de aparelho,
   registry de motores para compatibilidade de ports publicados, e nenhum
   suporte físico declarado sem gate no aparelho.
