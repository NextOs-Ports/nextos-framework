# Escolher o runtime de um jogo Android

[English](../en/ANDROID-RUNTIMES.md)

A entrada desta trilha é sempre uma build **Android** fornecida pelo dono; o destino é Linux ARM. Os exemplos de jogos vêm exclusivamente das fontes públicas já selecionadas no [catálogo](../../catalog/ports.json). Os guias acrescentam orientação; não importam outros ports ou dados comerciais.

## 1. Identificar antes de escolher

Execute os comandos na raiz do clone, com Python 3.11+, `readelf` e, para manifesto binário, `aapt`. O [inventário](INVENTARIO-ANDROID.md) explica inputs separados, limites e privacidade. Substitua o caminho e use um arquivo de saída novo:

```sh
python3 tools/inventory_apk.py /private/owner-input/game.apk --output work/android-inventory.json
python3 tools/find_reference.py --runtime gamemaker
python3 tools/find_reference.py --runtime renpy --abi arm64-v8a
python3 tools/find_reference.py --runtime haxe-lime --abi arm64-v8a
python3 tools/find_reference.py --runtime native-android --abi arm64-v8a
```

A busca retorna perfis de **origem Android por padrão**, com os caminhos PT/EN em `guides`. `--engine` procura nomes; `--runtime` seleciona a família, mesmo quando o nome da engine continua `unknown`. ABI e renderer são filtros adicionais, nunca substitutos de evidência. Resultado vazio pede investigar o contrato; não prova impossibilidade. `--platform all` permite consultar explicitamente o catálogo histórico completo, fora desta trilha.

## 2. Usar a trilha correspondente

| Família para `--runtime` | Indícios no input Android | Guia e referência selecionada |
| --- | --- | --- |
| `unity` | `libunity.so`; Mono ou IL2CPP/metadata | [Unity](../../portando_unity/README.md), casos por versão |
| `mono-android` | Mono/.NET Android, assemblies e bootstrap Java | [Mono Android](MONO-ANDROID.md), Stardew Valley, ScourgeBringer, Blossom Tales; SOR4 para adaptação de host |
| `godot` | Biblioteca Godot, PCK/export e versão correspondente | [Godot](GODOT.md), Tearscape |
| `cocos2d-x` | JNI/Cocos2d-x, scheduler e assets | [Cocos2d-x](COCOS2D-X.md), Chrono Trigger e Geometry Dash |
| `gamemaker` | `libyoyo.so`, runner e arquivos consumidos por ele | [GameMaker Android](GAMEMAKER-ANDROID.md), Forager |
| `renpy` | `librenpython.so`, bootstrap Python/Ren'Py e assets | [Ren'Py Android](RENPY-ANDROID.md), Summertime Saga |
| `haxe-lime` | `liblime.so` e `libApplicationMain.so` na mesma ABI; confirmar hxcpp | [Haxe/hxcpp/Lime Android](HAXE-LIME-ANDROID.md), Tightrope Theatre |
| `native-android` | Bibliotecas C/C++, JNI, NativeActivity ou SDL Android | [C/C++ Android](NATIVE-ANDROID.md), Action Squad, KOTOR, Angry Birds e RCRDX |

MonoGame/FNA entra somente quando pertence à build Android analisada. O inventário não identifica todos os runtimes: nomes renomeados, assemblies encapsulados e engines híbridas pedem leitura adicional. `liblime.so` isolada não comprova uma aplicação hxcpp; ausência de assinaturas conhecidas não basta para classificar C/C++. Um APK predominantemente Java/Kotlin exige investigar seu runtime; não há suporte ART genérico demonstrado por estes guias.

## 3. Entender o que a classificação prova

[Perfis](../../catalog/profiles.json) separam `platform`, `runtime_family`, engine, versão, ABI, renderer e status. As novas classificações apontam `classification_evidence` com caminho e SHA-256 do snapshot. O [registro de trilhas](../../catalog/android-runtimes.json) fornece os links bilíngues. Há 38 perfis de fontes Android; fichas comunitárias sem fonte importada não entram na busca.

Abra o `SOURCE-MAP.json` do resultado e confira commit, arquivos incluídos/omitidos e licença. Uma referência pode conter pins antigos ou posteriores à V5 e validação parcial. O Tightrope selecionado é `1.0.5-test.1`, com aceitação física exata pendente: serve para estudar a classificação e diagnosticar limites, não como baseline aprovado. Não promova seu workaround a padrão.

## 4. Implementar no escopo V5

Use o [SDK](../../toolchains/sdk/README.md), os [pins](PINS-E-FONTES.md), o [primeiro port autoral](../../examples/first-port/README.md) e um projeto separado para o adapter. Prefira AArch64 quando a build Android o oferecer. Não confunda CPU de um wrapper Linux com ABI do guest. Para ARMv7, prove a ponte softfp/ARMHF.

A V5 permanece em `framework-v5` no commit `657fb65a23b5c3b20040e76307b27e6470b1d17c`. Verifique os contratos do loader antes de integrar a referência. Um recurso exclusivo de V6 não fica disponível porque há um snapshot dele no catálogo; registre a dependência incompatível. Nenhum runtime compartilhado foi acrescentado por esta documentação.

## 5. Fechar o novo port

Preserve a ordem nativa e mantenha classes, callbacks e adaptações específicas no adapter. Use SDL do sistema e audite todos os ELFs Linux públicos para GLIBC ≤ 2.30. Os guias [NXExtract](NXEXTRACT.md) e [testes e entrega](TESTES-E-ENTREGA.md) descrevem instalação limpa, dados do dono, `INSTALLATION.md` bilíngue e provas do mesmo artefato final.

As telas NXExtract/NXSplash continuam canônicas. Imagem, áudio, controles, save/reload e saída precisam de evidência apropriada; a pesquisa de referências e um teste host não aprovam um aparelho. Preserve bytes já aprovados e reúna os testes necessários antes de uma abertura física autorizada.
