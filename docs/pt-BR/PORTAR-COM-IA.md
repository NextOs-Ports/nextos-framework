# Criar ports com ajuda de IA

A IA pode conduzir a maior parte da investigação, programação, compilação e verificação. O dono fornece a cópia compatível do jogo, define o alvo e confirma a experiência física que não pode ser demonstrada por logs.

## Uma missão inicial que você pode copiar

```text
Leia AGENTS.md, o catálogo e o guia de arquitetura deste repositório.
Quero criar um port Android para Linux AArch64, começando pelo alvo que informarei.
Vou fornecer localmente uma cópia do jogo. Não envie seus dados ao GitHub.

Faça o inventário de package ID, versão, ABIs, engine, bibliotecas, imports,
JNI, dados e requisitos gráficos. Prefira arm64-v8a quando existir.
Escolha referências do catálogo por engine, ABI e contratos comprovados.
Leia SOURCE-MAP.json, licenças e limitações antes de reutilizar qualquer peça.

Crie o novo port em diretório separado. Preserve a V5 e os ports de referência.
Implemente somente o adapter necessário, mantendo o fluxo nativo do jogo.
Compile com toolchain e sysroot explícitos, use testes dirigidos e continue
autonomamente nas etapas reversíveis autorizadas. Não invente sucesso em shims.

Registre cada etapa, as fontes reutilizadas e o resultado. Peça informação
somente quando um dado ausente realmente impedir o próximo passo.
Não publique, não altere visibilidade e não acesse aparelhos não autorizados.
```

## O trabalho que a IA deve entregar

1. Inventário técnico da cópia local, sem distribuir dados ou citar sua origem de download.
2. Plano de contratos: o que a V5 fornece, o que a engine exige e o que falta implementar.
3. Adapter, shims específicos e build em fonte versionada, com origem e licença de cada peça reutilizada.
4. Diagnóstico de imports/JNI, vídeo, áudio, entrada, save e saída, com falhas explícitas.
5. Receita NXExtract, launcher canônico, instalação limpa e documentação dos dados exigidos.
6. Evidência do artefato exato, separando teste host, emulação e teste físico.

Não basta a IA devolver uma lista de sugestões: ela deve escrever e compilar o código possível, executar verificações autorizadas e documentar os bloqueios reais. Um resultado parcial precisa ser chamado de parcial.

## Como escolher uma referência

Use `catalog/ports.json`. Para Unity, compare versão, pipeline, ABI e forma de input; para Cocos/native, compare lifecycle e imports; para MonoGame/.NET ou GameMaker, use a trilha própria. Consulte `docs/pt-BR/SHIMS.md` para localizar o código disponível.

Um menu renderizado não prova o jogo inteiro. Uma solução de áudio não autoriza copiar o lifecycle completo. Não aplicar automaticamente a todos os títulos os offsets, stubs, resoluções, backends ou controles de um jogo.

## O que continua dependendo de validação do dono

Informar dados e aparelho correto, confirmar comandos físicos/áudio/imagem e aprovar uma publicação pública. A IA pode preparar todos os materiais revisáveis antes dessa aprovação. O repositório atual permanece privado até uma ordem explícita do mantenedor.
