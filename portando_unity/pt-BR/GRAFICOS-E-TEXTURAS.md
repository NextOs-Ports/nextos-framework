# Gráficos, ETC1 e transparência

[English](../en/GRAPHICS-AND-TEXTURES.md)

O resultado desejado é uma imagem correta no contexto físico disponível. Um programa traduzido, um upload sem erro ou um FPS alto não substitui essa prova. Preserve a rota gráfica aprovada de cada port.

## 1. Separar quatro decisões

Registre a API serializada nos dados, os programas disponíveis, as operações lógicas solicitadas pela Unity e o contexto físico aberto no aparelho. Uma fachada pode traduzir um subconjunto necessário de GLES3 para GLES2; não pode anunciar capacidades sem implementar seus efeitos.

Em Unity com GLES2 nativo, preserve variantes e formatos já suportados. Se a build usa outros programas, identifique cada stage, parâmetros, atributos, samplers, constantes e operações incompatíveis. Compile e faça link, depois compare cenas reais. [Freedom Planet 2](FP2-VULKAN-GLES2.md) demonstra por que programas Vulkan exigem um pipeline específico.

## 2. Medir texturas residentes

Para cada objeto, registre arquivo/container + identidade do objeto, dimensão, mips, formato original e formato do upload, material consumidor, alpha real, updates e tempo de vida. Nome de textura ou PathID isolado não é identidade suficiente entre arquivos diferentes.

PNG ou um bundle comprimido mede armazenamento, não memória GPU. Observe criação, redefinição e exclusão; uploads acumulados não são residência simultânea. Separe cópia CPU, expansão temporária, encoder/cache, payload GPU e overhead do driver.

## 3. Selecionar ETC1/dual por uso

ETC1 armazena RGB em blocos de 4×4 com oito bytes por bloco; alpha exige outra representação. Essa base vem da [especificação Khronos ETC1](https://registry.khronos.org/OpenGL/extensions/OES/OES_compressed_ETC1_RGB8_texture.txt). Na estratégia dual, uma segunda imagem comprimida transporta o alpha e o shader o recompõe.

| Uso | Decisão inicial |
| --- | --- |
| Cor estática realmente opaca | Avaliar ETC1 RGB |
| Cor estática com blend/cutout | Avaliar dual com shader, sampler e mips coerentes |
| Cor premultiplicada | Manter convenção de RGB/alpha e blend |
| Fonte SDF, normal, LUT ou máscara numérica | Análise própria; não classificar como cor comum |
| Render target, depth, vídeo ou atualização dinâmica | Fora de conversão estática automática |
| Uso/alpha desconhecido | Investigar antes de converter |

ETC1 não exige reduzir dimensões. Preserve atlas, rects, UV, pivôs e sprites. [Sally Face](../cases/sallyface.md) mostra que cortar atlas pode parecer defeito do codec; [Horizon Chase](../cases/horizonchase.md) conserva camadas RGB/alpha existentes.

## 4. Calcular os bytes sem prometer FPS

Para cada nível, `ETC1 = ceil(w/4) × ceil(h/4) × 8`; some todos os mips, incluindo 2×2 e 1×1, que ainda ocupam um bloco por plano. Dual usa dois planos correspondentes. Uma textura muito pequena pode não economizar nada.

Em 1024×1024 com mips completos: RGBA8888 ocupa 5.592.404 bytes de payload; ETC1, 699.064; dual, 1.398.128. Esses valores são cálculo de payload, não RSS medido nem alocação total do driver. Use o [planejador offline](../diagnostico/texturas/README.md) para um inventário sintético ou local.

## 5. Provar upload e shader juntos

Observe definição completa, storage e subimagem. Só interceptar TexImage2D pode deixar um upload posterior expandir ou sobrescrever a textura. Um enum ETC2 não pode virar ETC1 sem provar o conteúdo dos blocos. Preserve format/storage/upload coerentes.

No dual, valide identidade RGB/alpha, dimensões, mips e intervalos do sidecar. RGB idêntico pode ter alpha diferente; não selecionar a primeira entrada pelo hash RGB. Acompanhe redefinição e exclusão do objeto GL. Meça unidades de textura realmente disponíveis e atualize todos os programas consumidores necessários.

No shader, preserve UV, filtragem/wrap, premultiplicação e threshold de cutout. Verifique bordas, texto pequeno e mips distantes. [Merchant of the Skies](../cases/merchantskies.md) ilustra um canal R8/alpha que precisa respeitar o contexto físico e o material.

## 6. Resolução e coordenadas

Meça painel/drawable, janela, render target, viewport lógico e retângulo de conteúdo separadamente. Reduzir a janela pode não reduzir os RenderTextures. Uma resolução virtual do framebuffer pode ser maior que a área visível.

Para letterbox, transforme o ponteiro pelo offset e escala do retângulo de conteúdo; trate cliques nas barras conforme o contrato, não como posição válida inventada. Posição e delta devem usar a mesma origem e escala. Confirme centro, quatro cantos, drag e release após resize.

Não imponha uma política de escala a um port já aprovado. [Oceanhorn](../cases/oceanhorn.md) preserva o framing; [Prizefighters 2](../cases/pf2.md) exige atenção à origem Y do Mouse managed.

## 7. Encerrar a mudança

Converta dados do dono em stage transacional, com ferramentas e parâmetros pinados. Compare objetos não alterados e reabra a saída. Depois meça qualidade, memória, tempo de carregamento e frame time na mesma cena. Uma redução teórica não autoriza anunciar ganho de FPS.
