# GameMaker Android e o runner YoYo

[English](../en/GAMEMAKER-ANDROID.md)

Esta trilha adapta a build Android ao Linux ARM, preservando seu runner e os dados fornecidos pelo dono. Comece pela [seleção de runtimes](ANDROID-RUNTIMES.md). A referência é o port público de [Forager](../../ports/forager-nextos/README.md), já incluído nesta coleção.

## 1. Inventariar a build Android

```sh
python3 tools/inventory_apk.py /private/owner-input/game.apk --output work/gamemaker-inventory.json
python3 tools/find_reference.py --runtime gamemaker
```

Confirme package ID, versão do jogo, ABIs, `libyoyo.so`, dependências C++, dados, páginas de textura, áudio e extensões Java/JNI. Registre a versão do runner separadamente. O GameMaker permite compilar Android por VM ou YYC; identifique a variante da cópia antes de escolher uma implementação. [Documentação oficial de compilação Android](https://gamemaker.io/en/help/articles/android-compiling-your-app).

Forager 1.0.13 é o perfil Android ARMv7 da referência. Esse número é a versão do jogo. Não extrapole para outro runner, nem escolha ARMv7 para outro jogo que ofereça `arm64-v8a`. `data.win` de desktop não é input substituto para esta receita Android.

## 2. Fixar as fontes e a licença

O [SOURCE-MAP](../../ports/forager-nextos/SOURCE-MAP.json) fixa `89e5c7107889f6b19a705f006f9f492bb0eb80da`. Leia:

- [Bootstrap e limites](../../ports/forager-nextos/upstream/README.md).
- [Fronteira do runner](../../ports/forager-nextos/upstream/source/overlay/gmloader/libyoyo.cpp) e [RunnerJNILib](../../ports/forager-nextos/upstream/source/overlay/gmloader/classes/RunnerJNILib.cpp).
- [Build histórico](../../ports/forager-nextos/upstream/source/build-release.sh), [preparação dos dados](../../ports/forager-nextos/upstream/tools/build_forager_port.py) e [receita NXExtract](../../ports/forager-nextos/upstream/extractor.json).

O [NOTICE](../../ports/forager-nextos/upstream/NOTICE.md) declara o loader derivado de gmloader-next em `c2fca354df73761887c15f44a0b28ec823581cd5`, sob GPL-2.0-only. A coleção contém código GPL-3.0-only: mantenha essa fronteira explícita e não cole esse loader em componentes V5 como se tivesse a mesma licença. Preserve notices e avalie a composição em projeto separado conforme [licenças da coleção](../../LICENSING.md). Runner e assets originais vêm do dono.

## 3. Preservar o fluxo do runner

Na referência, NXExtract prepara `runtime/forager.port`; o loader carrega `libc++_shared.so` antes de `libyoyo.so`, resolve imports e preserva inicializadores, JNI e loop nativo. Mapeie a sequência exata da nova build: classes Java, chamadas RunnerJNILib, superfície, dimensões, idioma, áudio, controles, pausa e encerramento.

Um arquivo `.port` é uma escolha do adapter histórico, não uma API universal do GameMaker ou da V5. Não chame uma sala, função do jogo ou entrypoint intermediário para contornar a inicialização. Extensões opcionais precisam de contrato de ausência e erro real; simular sucesso de um serviço obrigatório oculta falhas posteriores.

## 4. Construir o adapter e provar ABI

Use [compilação ARM](COMPILAR-ARM.md) para produzir o executável Linux em projeto próprio. Leia o build histórico antes de executar: fontes recuperadas, imagem/sysroot, bibliotecas e arquivos omitidos precisam estar fixados. Seu uso de nxbootstrap/NXExtract antigos não migra a referência para V5. Não reconstrua o binário aprovado para escrever o guia.

Em ARMv7, confira ARM/Thumb, calling convention, doubles/floats, callbacks e estruturas na ponte Android softfp/Linux ARMHF. A tradução deve cobrir o caminho de volta, não somente imports. C++ exige ownership, exceções e versão da biblioteca padrão; não passe objetos entre ABIs sem contrato. Imports desconhecidos precisam de erro verificável, nunca uma tabela genérica de `return 0`.

## 5. Diagnosticar as fronteiras do jogo

| Sintoma | Medição e contraprova |
| --- | --- |
| Falha antes do primeiro frame | Ordem das bibliotecas, construtores, JNI e primeira chamada não resolvida; rejeitar biblioteca/ABI errada |
| Sprites ausentes ou superfícies pretas | Shader real, textura, alpha, FBO e pixels antes do present; áudio sem frame deve falhar |
| Parte dos botões não responde | Máscara, press/release e dispositivo consumidos pelo runner; reconectar sem duplicar eventos |
| Idioma/save desaparece ao reiniciar | Caminho persistente separado dos dados extraídos; salvar, encerrar e recarregar sem reextração destrutiva |
| Áudio quebra após pausa | Formato, fila, callback e retomada; comprovar consumo e escuta no alvo |

Use SDL do sistema e provider GLES real. Ajustes de extensão, textura ou input de Forager permanecem específicos até prova em outro port. Os critérios de pixels, saves e áudio estão em [testes e entrega](TESTES-E-ENTREGA.md).

## 6. Preparar os dados e a entrega

A referência cria um arquivo STORE determinístico com 24 membros. Esses membros e hashes pertencem àquela build; o novo port precisa de inventário próprio. [NXExtract](NXEXTRACT.md) deve validar identidade Android e payloads críticos, preparar o layout completo e executar hooks em instalação limpa. O SHA do APK de referência identifica o teste, sem ser a única trava de compatibilidade do container.

Inclua `INSTALLATION.md` PT/EN com identidade técnica do input e mantenha saves fora do selo de dados. Audite todos os ELFs Linux públicos para GLIBC ≤ 2.30; preserve launcher V5, UI gráfica e NXSplash de cinco segundos. A prova histórica de Forager não aprova outro runner, aparelho ou bytes reconstruídos.

## Missão para a IA

```text
Analise somente a build Android fornecida. Identifique runner GameMaker,
VM/YYC, ABI, assets e extensões. Leia Forager no commit fixado e sua licença.
Documente o fluxo nativo e implemente o adapter separado, respeitando a V5.
Prove ABI, gráficos, áudio, input e persistência com testes dirigidos.
Prepare receita NXExtract completa e registre dependências incompatíveis.
Não importe dados comerciais nem transforme o loader histórico em padrão V5.
```
