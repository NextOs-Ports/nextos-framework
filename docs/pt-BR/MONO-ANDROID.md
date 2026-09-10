# Mono Android, MonoGame e runtimes .NET

[English](../en/MONO-ANDROID.md)

Pratique primeiro o [exercício Mono autoral](../../examples/engines/README.md): P/Invoke, layout, callback e erro testados no host Linux. Ele ensina essa fronteira; o bootstrap Mono Android descrito abaixo continua uma implementação distinta.

Este guia ensina a escolher a rota de execução de jogos Android com código gerenciado, preservar o bootstrap e diagnosticar as pontes nativas. Não existe uma receita única “copiar as DLLs e executar mono” que cubra todas essas builds.

## 1. Classificar o runtime real

| Evidência na cópia local | Hipótese a confirmar | Próxima decisão |
| --- | --- | --- |
| `libmonodroid`, `libmonosgen`, assembly store, classes `mono.android` | Mono/.NET para Android | Estudar a cadeia de runtime Android |
| `libunity` e assemblies gerenciados | Unity com Mono | Usar o guia Unity e sua versão exata |
| `libunity` + `libil2cpp` + metadata | Unity IL2CPP | Código gerenciado convertido em nativo; outra rota |
| Assemblies MonoGame/FNA e dependências desktop compatíveis | Possível host Linux gerenciado | Provar o entrypoint e cada dependência |
| Godot com C# | Godot/.NET | Seguir a versão de Godot e seu host .NET |

Esses nomes são indícios. Confirme versões, ABIs, dependências e forma de armazenamento dos assemblies. Assemblies podem estar em containers, comprimidos ou acompanhados por imagens AOT; a extensão `.dll` sozinha não diz se há IL suficiente para JIT.

## 2. Escolher entre manter o runtime Android e usar um host Linux

**Rota A: compatibilidade Android.** Carregar a cadeia nativa que pertence à build do dono, implementar Bionic/JNI/Android e permitir que o bootstrap original inicialize o runtime gerenciado e a Activity. [Stardew Valley](../../ports/stardewvalley-nextos/README.md) e [ScourgeBringer](../../ports/scourgebringer-nextos/README.md) oferecem código público para investigar essa rota.

**Rota B: host gerenciado Linux.** Só é válida quando entrypoint, assemblies, BCL e bibliotecas nativas realmente funcionam fora do ambiente Android, com adaptações documentadas. [SOR4](../../ports/sor4-nextos/README.md) e seus patches MonoGame mostram outras fronteiras; não transplantar o bootstrap de um jogo para outro por ambos usarem C#.

O [manual de embedding Mono](https://www.mono-project.com/docs/advanced/embedding/) descreve inicialização, carregamento de assemblies e chamadas managed/native para aplicações compatíveis. Essa API não substitui automaticamente `Java_mono_android_Runtime_init`, registro de classes Java, Activity ou empacotamento de uma build Android.

## 3. Mapear a inicialização antes de chamar Game.Run

Documente a ordem das bibliotecas e seus construtores/JNI, configuração do runtime, armazenamento de assemblies, registro de tipos/assemblies, Activity/OnCreate, criação da view e início do loop. No Stardew selecionado, o README descreve `libmonosgen-2.0.so → libxamarin-app.so → libmonodroid.so` e o bootstrap Android; essa ordem pertence àquele perfil.

Uma chamada precoce a `Game.Run()` pode pular inicialização necessária. Carregue dependências com o loader correto: um ELF Bionic não deve ser enviado cegamente ao `dlopen` glibc. Se uma referência recusa AOT para usar JIT, confirme que o novo runtime permite esse fallback e possui o IL necessário antes de reutilizar a decisão.

## 4. Resolver as fronteiras que costumam falhar

| Fronteira | O que medir | Exemplo de diagnóstico |
| --- | --- | --- |
| Bionic/glibc | Constantes, estruturas, alinhamento e errno | Enum `sysconf` errado informa tamanho de página inválido |
| Threads/GC | Semáforos, TLS, suspensão e sinais por ABI | Estrutura `sem_t` de tamanho incorreto corrompe memória |
| JNI/Java | Assinaturas, referências, classes e callbacks | Activity alcançada não prova serviço Java tardio |
| P/Invoke | Nome real, ABI e resolução de cada biblioteca | Provider errado aparece só ao abrir uma função do menu |
| Assemblies | Identidade, versão, integridade e dependências | Mistura de BCL/runtime quebra tipos ou métodos |
| EGL/GL | Contexto real e classificação desktop GL/GLES | Biblioteca desktop detectada indevidamente muda a rota MonoGame |

Comece em `src/bionic_shims.c`, `src/pthread_bridge.c`, `src/jni_shim.c` e `src/sdv_egl_bridge.c` do Stardew, e nos shims/ponte AAudio de ScourgeBringer. Confira os arquivos incluídos nos manifestos. Não substitua `sysconf` inteiro por constantes de outro aparelho nem desabilite GC/jobs como correção geral.

## 5. Fazer o primeiro build

Siga [compilação ARM](COMPILAR-ARM.md) para o executável Linux; depois leia o script de build do port escolhido e liste toolchain, fontes geradas e dependências faltantes. Um projeto C# com fontes pode ter etapa de build gerenciado separada; isso não recompila os assemblies proprietários do dono nem autoriza incluí-los no Git.

Fixe versões de runtime/BCL e bibliotecas nativas. Audite GLIBC de toda dependência Linux redistribuída. Para SOR4 público, a receita de referência é `port/wwise-native/build-glibc230.sh` no repositório `sor4-nextos`; não use uma receita histórica de outra árvore como equivalente.

## 6. Conferir vídeo, áudio e entrada

Preserve a API gráfica realmente escolhida pelo runtime e um dono do contexto/present. Som requer o backend específico: XACT/OpenAL, FMOD/AAudio ou outra rota não são intercambiáveis. Registre sample rate, formato, canais, callbacks e fila consumida, depois escute no aparelho.

Entregue gamepad à rota Android/MonoGame real e observe o consumidor gerenciado. Um evento SDL enfileirado não prova uma ação. Exercite menu horizontal/vertical, gameplay, pausa, teclado na tela, hotplug e saída; evite duplicar mouse/teclado/gamepad sem contrato.

## 7. Preparar os dados e fechar a evidência

NXExtract deve preparar assemblies/store, bibliotecas e conteúdo da cópia completa do dono. Uma migração necessária do store precisa acontecer também na instalação do zero, e preservar saves em atualização. [Receita e teste limpo](NXEXTRACT.md).

Registre o perfil de runtime, artefato/SHA, entrada na Activity, primeiro frame, áudio, input, save/reload e shutdown. O snapshot de ScourgeBringer registra limitações de validação de glibc 2.30: não transformar um teste host ou outro firmware em aprovação daquele alvo. Leia sempre o status do commit selecionado.

## Missão para a IA

```text
Identifique o runtime gerenciado e seus inputs completos. Compare as rotas
Android e Linux pelo contrato, não pelo nome .NET. Documente a cadeia de
bootstrap, Bionic, JNI, P/Invoke, assemblies e AOT/JIT. Implemente o adapter
em projeto separado, preserve dados e referências, faça build ARM e testes
dirigidos. Registre os contratos ainda não provados e a instalação NXExtract.
```
