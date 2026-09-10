# Triagem de uma build Unity

[English](../en/TRIAGE.md)

Esta edição seleciona o material de `portando_unity` para os ports NextOS admitidos no catálogo. Comece pela identidade real da build; a versão da Unity, sozinha, não decide renderer, áudio ou input.

## 1. Identificar sem adivinhar

Registre nome/versão do jogo, package ID, conjunto completo de containers, ABIs, biblioteca Unity, runtime Mono/IL2CPP e versão completa da engine. Cruce cabeçalhos de assets e identificação no runtime. Uma pasta chamada unity6_shader.c não prova Unity 6, e “Unity 22” não substitui 2022.x.y com seu sufixo.

Separe observação estática, hipótese e comportamento medido. Se não encontrou o patch exato, registre a lacuna. Use hashes dos payloads críticos; o nome externo do APK não identifica a build.

## 2. Inventariar as fronteiras

| Área | Campos a levantar |
| --- | --- |
| Boot | Bibliotecas, construtores, JNI_OnLoad, NativeLoader e entrada da Activity |
| Runtime | Mono/IL2CPP, metadata/assemblies e versão |
| Gráficos | Built-in/URP, API serializada, shaders/variantes, contexto físico e extensões |
| Texturas | Formato residente, dimensões, mips, alpha, uso e updates |
| Áudio | FMOD, OpenSL, AudioTrack, AAudio; chamadas, formato e callbacks |
| Input | InControl, Rewired, Input System, código próprio ou touch; consumidores |
| Dados | StreamingAssets, bundles, split/OBB, layout e preparação necessária |
| Estado | Preferências, save, pause/resume e saída |

O APK pode conter variantes que não são selecionadas. Diferencie o formato armazenado do formato realmente enviado à GPU. Avalie ETC1/dual em toda triagem Mali-450, com exceções documentadas em [texturas](GRAFICOS-E-TEXTURAS.md).

## 3. Escolher referências por perfil

Use o [índice Unity](../README.md) e os hashes de [SOURCE-MAP.json](../SOURCE-MAP.json). Compare versão, ABI, problema e contrato, depois consulte a implementação pública. Não trazer casos fora da seleção para esta edição.

Exemplos: Suzy Cube ajuda com GLES2/ETC1 nativos e lifecycle 2017; Freedom Planet 2 com tradução de programas Vulkan; Huntdown exige separar seus dois perfis; Party Hard explica autoridade direcional; Nameless Cat mostra integração com SDL do firmware.

A documentação de uma solução histórica não vincula automaticamente o HEAD público ao binário aprovado. Registre a diferença e peça a evidência exata somente quando ela for necessária para a adaptação.

## 4. Preservar o fluxo Android

Documente ordem de carregamento, relocações, construtores, JNI, configuração de surface/foco/resume e entrada no frame nativo. O adapter acompanha o fluxo usado pela build. Não chamar métodos “parecidos” de outra versão ou pular inicialização para alcançar uma cena.

Não retornar handles genéricos a bibliotecas ausentes. Uma falsa disponibilidade de AAudio ou um objeto JNI inválido pode adiar a falha até áudio ou carregamento. Ausência opcional segue o erro previsto; ausência obrigatória vira bloqueio identificado.

## 5. Emitir um resultado acionável

Entregue identidade, referências escolhidas, tabela de contratos, primeira fronteira suspeita, medição para confirmar/refutar e próximo ajuste local. “Viável” deve vir acompanhado do que existe e do que falta implementar; não equivale a port jogável.

O alvo Mali-450 usa GLES2 físico. `-force-gles20` não cria variantes, shaders ou operações ausentes. Sem prova de pixels, som, ações, save e saída, o resultado continua parcial nos aspectos correspondentes. Use o [diagnóstico por fronteira](DIAGNOSTICO.md).

## Missão para a IA

```text
Confirme a build Unity e faça inventário por fronteira. Use as referências
NextOS admitidas no catálogo e fixe hashes/licenças. Preserve fluxo nativo,
SDL do firmware e GLES2 físico. Planeje texturas ETC1/dual por uso, implemente
somente o adapter do novo port e registre medição, reparo e contraprova.
```
