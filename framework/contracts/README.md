# Contratos compartilhados do framework

[`declarative-v1.json`](declarative-v1.json) é o lock machine-readable das três
camadas e das versões compatíveis. Ele impede que uma conveniência de um port seja
silenciosamente promovida a comportamento global.

## Três camadas

- `pre-main` pertence ao `nxbootstrap`, com `nxsplash` como processo auxiliar:
  PortMaster, ABI/`PT_INTERP`, NXExtract, tela de identidade depois do payload,
  bibliotecas declaradas, lock e supervisão do filho direto;
- `runtime-common` pertence a `nxcompat`, `nxgl`, `nxinput`, `nxloader`,
  `nxandroid` e `nxobs`: probe, plano process-local, abertura/relatório real,
  mapping, imports explícitos, validação do perfil de lifecycle e observação
  opt-in de crash sem engolir o sinal nativo;
- `adapter` pertence ao port: lifecycle Android/engine, JNI, saves, UI, shutdown e
  quirks específicos com evidência.

O shell não pode receber fluxo nativo da engine, tabela de device, backend forçado ou
administração do frontend. O núcleo comum não pode deduzir capacidade pelo nome do
firmware. O adapter não pode pular `init_array`, `JNI_OnLoad` ou etapas originais.

## Versões

Os componentes `nxcompat`, `nxgl`, `nxinput`, `nxloader`, `nxandroid` e `nxobs`
usam API versionada; os componentes com structs públicos também fixam
`struct_size`. Compatibilidade precisa ser explícita. `nxandroid`
valida fronteiras e ordem, mas não fornece uma VM/JNI genérica nem inventa
callbacks da engine. `nxgl` 0.2 mantém a API 1 literal e publica a API 2 como
versão corrente, com ownership do stack e callbacks congelado até `close`.
O conjunto gerado pelo `nxbootstrap` — launcher, `nxport.json` e o ELF
`nxsplash-nextos` correspondente à arquitetura — é pinado como uma unidade
exata. Na RC7, NXRelease 0.2.41 exige nxbootstrap 0.6.36,
nxgenerator 0.2.19, NXExtract 1.2.21, schema público 3, geração runtime v2 e o
ciclo PortMaster real. Schemas 1/2 permanecem entradas legadas sem promoção
silenciosa para a closure v2. Cada release é um
conjunto de cinco conteúdos pinados — incluindo a UI obrigatória —, não apenas
um nome de versão. O ELF da UI é selecionado pelo manifesto multiarch canônico
conforme a arquitetura do `nxport.json`; o runner público só inicia a varredura
dos dados depois de um renderer gráfico atestar a interface preservada.

Após essa RC7 imutável, a versão corrente do NXRelease é 0.2.43. Seu modo
`public-final` pode reproduzir os bytes exatos de um candidato autenticado cuja
metadata interna declare 0.2.40, 0.2.41 ou 0.2.42, preservando a versão do artefato em
`NXRELEASE-METADATA.json` e `SBOM.cdx.json`. Não existe override externo: o ZIP
testado e as builds A/B continuam obrigatoriamente iguais, enquanto o gate e
`BUILD-PROVENANCE.json` registram o NXRelease executor 0.2.43. Builds ordinárias
usam 0.2.43; essa compatibilidade não reinterpreta nem move as tags RC7.
Novos candidatos usam nxbootstrap 0.6.37 e nxgenerator 0.2.20: modos POSIX
alterados pelo instalador só são restaurados depois da autenticação integral
do store. O controle `commit` permanece obrigatório e nunca é recriado.

O contrato host-side `nextos-framework-build-pin-v1`, em
[`../nxgenerator/schema/framework-build-pin-v1.schema.json`](../nxgenerator/schema/framework-build-pin-v1.schema.json),
fecha a fonte anterior ao compilador. Cada componente declara commit Git completo,
`VERSION` e digest canônico de modo/path/tamanho/SHA-256 dos blobs. O nxgenerator
recalcula a identidade SHA-1 de commit, árvores e blobs, publica um snapshot novo
sem overwrite, reabre o recibo `FRAMEWORK-SOURCE.json` e o build usa somente essa
árvore em modo read-only. O contrato histórico de release não é reinterpretado;
NXExtract continua no pin próprio de `GENERATION.json`.

O gate compara cada `current_version` com o arquivo `VERSION` real e cada API C com o
header público correspondente.

## `nxport.json` v3

O schema atual está em
[`../nxbootstrap/schema/nxport-v3.schema.json`](../nxbootstrap/schema/nxport-v3.schema.json).
Ele preserva as fronteiras tipadas do v2 e acrescenta a closure opt-in
`generation_runtime`, com papéis, paths, modos e hashes exatos para runtime e
NXExtract. O contrato inclui:

- objeto `nxextract` com pin exato `1.2.21` na RC7;
- `private_library_paths`, separados do firmware/PortMaster;
- `generation_runtime`, obrigatório para a geração v2 pública;
- `required_capabilities` nos namespaces `host`, `graphics`, `audio` e `input`;
- `enabled_quirks` nos namespaces `adapter`, `engine` e `game`, vazio por padrão;
- `runtime_report` obrigatório em `log` ou `log-and-logo`.

Nenhum nome pode usar `device.*`. Capabilities declaram fatos que o adapter ainda
precisa comprovar; quirks apenas habilitam código específico já implementado e
documentado. Essas listas nunca definem variável SDL/Mesa nem selecionam correção por
modelo.

`home_mode: preserve` é o default. `port` só entra com prova da engine. Save/cache
adicionais continuam responsabilidade contida do adapter; o manifesto genérico não os
redireciona por firmware.

Entradas v1/v2 continuam aceitas somente no fluxo legado e preservam a geração
control-only. A saída pública nova usa v3; o release rejeita uma promoção
generation-v2 sem a closure v3 completa. Campos desconhecidos — inclusive o antigo
`process_names` — falham fechado.

## Gates

```sh
python3 -B framework/nxbootstrap/tests/test-manifest-contract.py
bash framework/nxbootstrap/tests/run-isolated.sh
bash framework/nxrelease/tests/test_nxrelease.sh
python3 -B framework/nxgenerator/tests/test_framework_pin.py
```

O primeiro gate não cria processos de port. O segundo executa lifecycle/gerador
somente num PID namespace privado. O terceiro constrói ELFs sintéticos e prova que o
release compara o manifesto corrente com os assignments reais do launcher visível.
