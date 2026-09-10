# Simulação de um primeiro port

[English](ONBOARDING-SIMULATION.en.md)

**Registro histórico:** as lacunas abaixo foram observadas no commit indicado. A [atualização posterior](ONBOARDING-UPDATE.md) registra o SDK, exemplos e ferramentas implementados, seus testes e as pendências atuais.

Avaliação de 10/09/2026 sobre o commit `2e7234265f8602d8a31117f65a23874769e13e59`. Cenário: uma pessoa ou IA recebe somente esta coleção e sua própria cópia de um jogo Android, sem conhecer o ambiente interno NextOS.

**Conclusão: a coleção permite começar a investigação e compilar o exemplo host. Ainda falta um caminho reproduzível completo do clone até um port Android demonstrável.** O material é útil como referência, mas a IA ainda precisa descobrir e implementar a ligação entre várias etapas. “Guias completos” descrevia o objetivo editorial com confiança excessiva.

## 1. O que foi realmente simulado

Uma exportação limpa dos arquivos versionados, criada por `git archive` dentro de `work/`, foi usada para testar os comandos iniciais. Ela não contém os builds ou arquivos ignorados do workspace original. Foi usado o computador Linux existente, com GCC 16.2.1; não foi um container recém-instalado nem uma máquina ARM. O teste de pins usou os objetos Git do próprio repositório da coleção.

| Etapa | Resultado observado | Alcance |
| --- | --- | --- |
| `publication/verify.py` | PASS: 7.725 hashes | Integridade da seleção |
| CMake/build/CTest de `examples/shims-reference` | PASS: 1/1 | Exemplo C no host |
| Gerador com `nxproject-aarch64.example.json` | Diretório gerado com sucesso | Esqueleto de projeto |
| `recipe-check` com `recipe-minimal.json` | OK | Estrutura da receita |
| Pin de `nxloader` usando o commit histórico V5 | Falhou: `Git object query failed: fatal: Needed a single revision` | Objeto histórico não disponível nesta coleção |
| Pin de `nxloader` usando o commit da coleção acima | Create/materialize/verify PASS | Uma composição de um componente, sem build nem release |

O adapter gerado contém `status: unimplemented_nonrelease`, `release_ready: false`, lifecycle vazio e nenhum método JNI, callback de áudio ou ação de input. O executável de jogo declarado não foi criado. Isso corresponde ao contrato intencional do gerador; falta o exercício seguinte que implemente esse esqueleto.

Nenhum APK comercial, jogo, aparelho, build ARM/NDK, extração completa, suíte histórica ou empacotamento de release foi executado. Nenhum runtime V5 ou snapshot de port foi alterado.

## 2. Bloqueios prioritários

| Prioridade | Onde o iniciante para | Evidência e material necessário |
| --- | --- | --- |
| P0 | Obter um ambiente ARM reproduzível | [Compilar ARM](../docs/pt-BR/COMPILAR-ARM.md) requer compilador/sysroot externos e usa caminhos `/opt/...` exemplificativos. Falta uma receita pública com versões, origem, hashes, dependências C/C++/SDL/EGL e teste de baixa glibc. |
| P0 | Fixar os componentes a partir deste repositório novo | O [helper de pins](../framework/nxgenerator/framework_pin.py) exige objetos Git locais. O SHA da V5 original identifica a origem, mas não existe no histórico novo. Falta documentar e validar a composição completa a partir do commit exportado, mantendo também a procedência original. |
| P0 | Passar do exemplo C ao guest Android | [shims-reference](../examples/shims-reference/README.md) não carrega ELF Android. O exercício NDK compila uma função de soma, mas não mostra como o loader Linux a carrega, resolve imports e chama. Falta um projeto demonstrativo integrado. |
| P0 | Instalar os dados do exemplo pelo NXExtract | A receita mínima passa no parser, mas não acompanha um input autoral reproduzível e um tutorial de extração, hooks, validação e rollback. Falta ligar essa receita ao mesmo exemplo integrado. |
| P1 | Construir a referência escolhida | A seleção não é um checkout integral de cada port. O [build do Chrono](../ports/chrono-nextos/upstream/build_universal.sh) exige a imagem local `playfetch-builder:buster`, headers de um build NextOS e `fonts/NotoSans-Regular.ttf`, ausente da seleção. O [README histórico do FP2](../ports/fp2-nextos/upstream/README.md) manda executar `build_universal.sh`, também ausente. Falta uma lista explícita de dependências/arquivos omitidos e como recuperar cada fonte pública permitida. |

Não se deve recriar ou mover a tag histórica V5 para resolver o pin. A experiência bem-sucedida com o commit exportado prova que existe um caminho com o helper atual, mas somente `nxloader` foi materializado neste estudo. Os componentes de testes tiveram omissões; não declarar a composição exportada inteira idêntica à árvore histórica completa.

## 3. Guias e exemplos que ainda faltam

1. **Inventário Android executável.** O guia de IA lista bibliotecas no APK, mas deixa o leitor escolher como interpretar o manifesto binário. Acrescentar comandos/ferramentas pinados para package ID, versão, splits, ABI, engine, imports, tipos de símbolos e relocações. Gerar relatório privado e uma saída publicável sem caminhos pessoais. Explicar cedo que TLS/TLSDESC, RELR, IFUNC e packed relocations estão fora do contrato do [nxloader V5](../framework/nxloader/README.md), evitando descobrir essa incompatibilidade depois de escrever o adapter.
2. **Integração de shims por contrato.** O exemplo registra dois símbolos (`__errno`, `__android_log_write`) e uma propriedade didática; não cobre a maioria dos jogos. Mostrar a ligação ao registry real, um import obrigatório ausente, ownership e erro. Acrescentar exercícios separados de assets/leitura, estruturas Bionic, threads, JNI e referências por thread, lifecycle, áudio e input. Reusar implementações existentes somente quando seus contratos coincidirem.
3. **Projeto inicial para cada família.** Unity, Mono Android, Godot e Cocos2d-x já possuem guias de investigação. Ainda faltam exercícios autorais pequenos com arquivos completos, dependências, comando de build e saída esperada. Mono precisa distinguir um host gerenciado Linux de um bootstrap Android; Godot precisa distinguir exportar um projeto autoral de adaptar a build Android do dono. Nenhum deles deve prometer que copiar a referência basta.
4. **Catálogo pesquisável por perfil.** As 41 entradas de fontes têm origem/hash, mas nenhuma possui campos estruturados `engine`, `abi`, `renderer`, `build_status` ou `tested_devices`. Parte disso está nas fichas e nos 15 casos Unity. Consolidar os dados comprovados e usar `unknown` para o restante, permitindo à IA escolher uma base por contrato e não pelo título.
5. **Diagnóstico com resultados demonstrados.** Já existe uma tabela de problemas comuns. Acrescentar exemplos de saída boa/ruim: relocation recusada, import ausente, ABI incorreta, JNI não registrado, contexto inválido, áudio sem vídeo, save e encerramento. Cada caso precisa indicar a próxima medição e a condição de sucesso.
6. **Receita de entrega completa do exemplo.** Fornecer `nxproject.json`, contrato implementado, receita NXExtract, `INSTALLATION.md` PT/EN e comando que coordene a bateria final. A prova física continua específica do alvo; o projeto sintético não certifica jogos comerciais.

Os exemplos didáticos e a documentação podem viver fora das árvores congeladas. Uma lacuna que exija mudar código ou comportamento compartilhado da V5 pertence ao desenvolvimento V6 separado, não a um hotfix no baseline.

## 4. Atenção específica ao Freedom Planet 2

O [estudo Vulkan → GLES2](../portando_unity/pt-BR/FP2-VULKAN-GLES2.md) explica a transformação e as limitações. Para torná-lo reproduzível como aula, falta um laboratório independente dos dados comerciais:

- Mostrar como obter e construir as dependências do [tradutor](../ports/fp2-nextos/upstream/tools/build_exact_shader_tool.sh), respeitando o commit SPIRV-Cross, a versão SPIRV-Tools e os hashes SMOL-V já exigidos pelo script. Elas não estão todas fornecidas pela coleção.
- Usar shaders autorais simples para exercitar SMOL-V/SPIR-V → ESSL 1.00, bindings, alpha e rejeição de uma operação incompatível, com comandos e resultados esperados.
- Separar o teste host de tradução da compilação/link GLES2 e da imagem no aparelho. Uma amostra autoral não prova a transformação de toda a build FP2.
- Explicar como recuperar os arquivos públicos de build que ficaram fora da seleção e como aplicar a receita à cópia do dono, sem distribuir os shaders ou dados comerciais.

Esse laboratório deve ensinar o método de conversão. Não deve copiar os offsets, contagens de variantes ou exceções do FP2 como regra para outros jogos.

## 5. Ordem de implementação recomendada

1. Fechar o ambiente público ARM e o procedimento de pins da coleção.
2. Criar um **minijogo autoral de treino**, com gráficos e som gerados por código: guest Android AArch64, loader Linux, poucos shims explícitos, JNI/lifecycle, controle, save e saída. Publicar todas as fontes autorais.
3. Ensinar sua instalação NXExtract do zero e a geração do launcher, preservando a UI e a NXSplash canônicas.
4. Automatizar os testes host e cross reproduzíveis desse projeto e documentar a prova física separadamente. ARMv7 entra como exercício posterior com fronteira softfp explícita.
5. Expandir para exemplos de engine, laboratório de shaders FP2 e matriz de cobertura do catálogo.

Critério de conclusão: alguém em um ambiente independente consegue seguir PT ou EN, compilar, preparar o input autoral, gerar o projeto e localizar qualquer dependência faltante sem consultar arquivos privados. Cada etapa tem um resultado verificável e um limite declarado. A coleção permanece privada enquanto a revisão prossegue.
