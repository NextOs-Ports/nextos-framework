# Ren'Py Android: Python, SDL e bootstrap

[English](../en/RENPY-ANDROID.md)

Esta trilha parte da versão Android e usa como estudo as fontes públicas de [Summertime Saga](../../ports/summertimesaga-nextos/README.md), já selecionadas na coleção. O objetivo é executar o fluxo Android no Linux ARM com um adapter próprio. Consulte primeiro a [escolha de runtimes](ANDROID-RUNTIMES.md).

## 1. Identificar engine e dados completos

```sh
python3 tools/inventory_apk.py /private/owner-input/game.apk --output work/renpy-inventory.json
python3 tools/find_reference.py --runtime renpy --abi arm64-v8a
```

Confirme `librenpython.so`, ABI, dependências, Python embarcado, versão Ren'Py, biblioteca padrão, módulos nativos, scripts, archives e assets. Separe versão do jogo, versão dos scripts do runtime e identidade do ELF. O snapshot contém fontes Ren'Py 8.5.3; isso não certifica qualquer `librenpython.so` ou jogo Ren'Py.

Não misture automaticamente scripts/bytecode, extensões e intérprete de versões diferentes. A receita deve partir do APK e dos splits completos necessários. Copiar um diretório de uma edição desktop ou substituir o intérprete pelo Python do host não comprova compatibilidade Android.

## 2. Ler a referência exata

O [SOURCE-MAP](../../ports/summertimesaga-nextos/SOURCE-MAP.json) fixa `bf5929bfdcdb03e17a27d053a91c36477b207032`. Leia o [README histórico](../../ports/summertimesaga-nextos/upstream/README.md), a [licença](../../ports/summertimesaga-nextos/upstream/LICENSE) e os seguintes arquivos antes de adaptar:

| Fronteira | Fonte selecionada |
| --- | --- |
| Loader, SDL/JNI e entrada | [src/main.c](../../ports/summertimesaga-nextos/upstream/src/main.c), [jni_shim.c](../../ports/summertimesaga-nextos/upstream/src/jni_shim.c) |
| Bootstrap Python | [main.py](../../ports/summertimesaga-nextos/upstream/main.py), [android/apk.py](../../ports/summertimesaga-nextos/upstream/android/apk.py), [jnius](../../ports/summertimesaga-nextos/upstream/jnius/__init__.py) |
| Assets e transformação | [prepare_summertime_data.py](../../ports/summertimesaga-nextos/upstream/tools/prepare_summertime_data.py), [extractor.json](../../ports/summertimesaga-nextos/upstream/extractor.json) |
| Provider gráfico e áudio | [egl_shim.c](../../ports/summertimesaga-nextos/upstream/src/egl_shim.c), [audio_backend_policy.c](../../ports/summertimesaga-nextos/upstream/src/audio_backend_policy.c) |

Preserve licenças por arquivo, inclusive de Ren'Py e dependências. O README descreve resultados históricos por alvo; eles não substituem os limites do perfil ou aprovam a nova build.

## 3. Reproduzir o bootstrap Android

O `src/main.c` selecionado carrega o ELF Android com imports apropriados, prepara JavaVM/JNI, registra a integração SDL, fornece ambiente/dimensões e entra por `nativeRunMain`/`SDL_main`. `main.py` conduz o bootstrap Ren'Py. Mapeie construtores, `JNI_OnLoad`, callbacks e threads da build real antes de chamá-los.

Ren'Py Android permite chamadas Java via Pyjnius e acesso à `PythonSDLActivity`. Portanto, inventarie também chamadas feitas pelos scripts após o menu inicial. [Documentação oficial Android/Pyjnius](https://www.renpy.org/doc/html/android.html#pyjnius). Um módulo `jnius` substituto precisa implementar a assinatura, o objeto retornado e os erros realmente necessários; um objeto que aceita qualquer método não estabelece compatibilidade.

## 4. Separar layout, runtime e persistência

A preparação histórica remove prefixos de assets, monta um índice determinístico e aplica módulos de compatibilidade específicos do jogo. Leia as regras antes de reutilizar: não remova prefixos de todos os nomes indiscriminadamente. Detecte colisões após transformação, caminhos inválidos, arquivo obrigatório ausente e payload incompatível antes de publicar o diretório transacional.

Mantenha scripts/runtime pinados separados de saves, preferências e persistência do usuário. Teste save/reload, rollback quando usado, texto/acentos, entrada de nome e atualização sem apagar progresso. Os dois módulos de `runtime-overrides/` pertencem a Summertime Saga; não viram patches genéricos para todos os jogos Ren'Py.

## 5. Compilar e diagnosticar

Use [compilação ARM](COMPILAR-ARM.md) e o [SDK público](../../toolchains/sdk/README.md) para o loader Linux. O [build universal histórico](../../ports/summertimesaga-nextos/upstream/build_universal.sh) é material para inventariar fontes, sysroot, bibliotecas e dependências omitidas; não um comando garantido no clone selecionado. Confira todos os ELFs Linux públicos contra GLIBC ≤ 2.30 e use SDL do firmware. SDL incorporada ao ELF Android do dono não autoriza redistribuir uma SDL Linux privada.

| Sintoma | Próxima prova |
| --- | --- |
| Erro de import ou bytecode | Versão do intérprete/módulo, paths e arquivos do input; rejeitar conjunto de versões incompatível |
| Menu abre, cena falha | Assets completos, índice, import tardio e Java/Pyjnius realmente chamado |
| Áudio ativo e tela preta | Contexto/surface, shader, textura e pixels antes do present; invalidar vídeo preto |
| Som some após pausa | Backend negociado, formato/fila e retomada; não fixar configuração de outro aparelho |
| Toque/cursor erra o alvo | Viewport, transformação de coordenadas e consumidor Ren'Py; testar bordas e mudanças de resolução |

## 6. Instalar do zero e registrar limites

Implemente a receita [NXExtract](NXEXTRACT.md) para o input Android completo, identidade de package/versão/ABI e hashes dos payloads críticos. Aceite reempacotamento compatível sem usar o SHA integral como única condição. `INSTALLATION.md` PT/EN identifica a cópia de referência por campos técnicos e explica o destino dos arquivos.

Preserve UI do extrator e NXSplash canônicas. Registre extração real, hashes finais, imagem, áudio, input, persistência e saída conforme [testes e entrega](TESTES-E-ENTREGA.md). Um teste Python no computador não aprova a cadeia Android, o render Mali-450 ou outro device. Modificações específicas permanecem no adapter; a V5 fica congelada.

## Missão para a IA

```text
Inventarie somente o input Android Ren'Py fornecido. Fixe versões de Python,
Ren'Py, ELF, módulos e assets; leia Summertime Saga no commit selecionado.
Mapeie bootstrap SDL/JNI/Python e chamadas Pyjnius, inclusive as tardias.
Implemente contratos e preparação transacional em um projeto separado.
Teste erros, persistência e extração completa; registre os limites físicos.
Não misture runtimes desktop nem incorpore dados do jogo à publicação.
```
