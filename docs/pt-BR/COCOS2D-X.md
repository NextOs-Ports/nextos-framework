# Cocos2d-x Android em Linux ARM

[English](../en/COCOS2D-X.md)

Cocos2d-x costuma concentrar a engine e a lógica numa biblioteca C++, mas também depende de Java/JNI para lifecycle, assets, texto, áudio, serviços e entrada. O objetivo é reconstruir essas fronteiras mantendo o loop e a ordem nativa do jogo.

## 1. Confirmar engine e ABI

Procure símbolos `cocos2d`, classes `org.cocos2dx.lib`, métodos JNI de renderer e dependências C++. Confirme versão/fork, ABI, biblioteca principal, `libc++_shared.so`, assets e áudio. Jogos Cocos Creator ou forks com JavaScript podem ter uma arquitetura diferente; não classificá-los automaticamente como o mesmo Cocos2d-x nativo.

Use `readelf -h`, `readelf -d` e `readelf -Ws` nas bibliotecas locais. Relacione imports e callbacks que também são registrados dinamicamente. AArch64 é preferido quando existe; ARMv7 requer revisão de ABI, estruturas e chamadas com ponto flutuante.

## 2. Estudar uma referência pública com alcance definido

[Chrono Trigger](../../ports/chrono-nextos/README.md) documenta Cocos2d-x 3.14.1/GLES2, loader AArch64, `Cocos2dxBitmap`, OpenSL ES e controle nativo. [Geometry Dash/SubZero](../../ports/geometrydash-nextos/README.md) fornece outra seleção de fontes e perfis distintos.

Leia `SOURCE-MAP.json`, README, `src/main.c`, `src/jni_shim.c`, `src/text_render.c`, `src/opensles_shim.c` e `src/ct_framework.c` do Chrono. Os nomes específicos, hooks e pins pertencem àquele port. O catálogo não torna toda a combinação automaticamente V5.

## 3. Reconstruir a ordem de boot

No perfil Chrono documentado: dependência C++ carregada e inicializada; biblioteca do jogo carregada/relocada; hooks comprovados; construtores exatamente uma vez; `JNI_OnLoad`; contexto/assets; `nativeInit`; resume; loop de eventos/render; pause/save e saída.

Confira a ordem usada pelo Java e JNI da build de destino. O [Cocos2dxRenderer upstream](https://github.com/cocos2d/cocos2d-x/blob/v3/cocos/platform/android/java/src/org/cocos2dx/lib/Cocos2dxRenderer.java) é uma referência de como o lado Android dirige callbacks; a versão vendorizada pelo jogo prevalece. Não chame `nativeInit` antes de disponibilizar o contexto que ele utiliza, nem execute construtores duas vezes.

## 4. Montar o adapter por fronteira

| Fronteira | Implementação a provar |
| --- | --- |
| Assets | Busca pelo caminho/case correto, arquivos compactados, leitura/seek/fechamento |
| JNI | Classes, assinaturas, strings/arrays e callbacks reais; falha explícita para ausentes |
| Texto | Fonte, tamanho, alinhamento, Unicode, pitch/formato e alpha do bitmap |
| GLES | Contexto correto na thread de render, estado e único present |
| Áudio | Objetos/vtables OpenSL, BufferQueue e duração dos buffers |
| Controle | Gamepad/touch/teclado que o jogo efetivamente consome |
| Persistência | Diretório gravável, preferências, pausa, save e shutdown |

`Cocos2dxBitmap` merece um teste próprio: texto invisível pode vir de tamanho, stride, canal alpha ou callback de upload, mesmo que outros sprites apareçam. Não use uma string vazia ou bitmap fixo para ocultar método ausente.

## 5. Compilar sem misturar runtimes C++

O loader é Linux e segue o [guia ARM](COMPILAR-ARM.md); a biblioteca original continua Android. A dependência C++ do guest deve seguir o contrato da build, com resolução controlada para não colidir com o runtime do host. Não substitua arbitrariamente `libc++_shared.so` por `libstdc++` Linux.

Leia `build_universal.sh` do Chrono como receita específica, verificando imagens/SDKs e fontes faltantes antes de executar. No novo port, preserve flags, arquitetura, relocações, visibilidade e GLIBC auditadas. Não execute empacotamento enquanto estiver descobrindo imports.

## 6. Diagnosticar a primeira imagem

Se a engine não desenha, confirme que chegou ao render nativo, que há dados completos e contexto corrente. Se desenha mas fica preto, compare shader/link, textura/alpha, FBO e pixels imediatamente antes do present. Um objeto preto com silhueta correta pode ser sampler/wrap incorreto; não impor CLAMP global a todos os materiais.

Se o RGB está correto mas o scanout fica transparente/preto num compositor, teste a hipótese de alpha final sem destruir RGB ou o estado de scissor/clear. Qualquer reparo deve ser condicionado à capacidade/defeito medido, não apenas ao nome de um aparelho.

## 7. Testar comportamento completo

Teste texto nos idiomas disponíveis, menus, carregamento de fase, gameplay, transições, áudio, pause/resume, controle nativo, hotplug, save/reload e saída. Não sobreponha um mapper de teclado que roube o controle nativo. No touch, transforme coordenadas conforme o retângulo de conteúdo, com press/move/release e contexto coerentes.

Prepare [NXExtract](NXEXTRACT.md) para assets e bibliotecas críticas da cópia do dono, com validação real e sem dados embarcados. Registre [a prova do artefato final](TESTES-E-ENTREGA.md). Um menu traduzido ou um primeiro frame não prova todas as cenas.

## Missão para a IA

```text
Identifique o fork Cocos2d-x, ABI, biblioteca C++, JNI e ciclo do renderer.
Use Chrono e Geometry Dash como referências públicas por contrato. Implemente
assets, texto, áudio e controle no adapter novo, preservando construtores e
lifecycle. Faça build Linux ARM, testes dirigidos e receita NXExtract sem dados.
```
