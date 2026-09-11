# Inventário Android e escolha da referência

[English](../en/ANDROID-INVENTORY.md)

Comece pelo input compatível fornecido pelo dono. A ferramenta gera informações técnicas sem executar código do guest e sem incluir nome externo dos APKs ou caminhos do computador no JSON.

## 1. Executar o inventário

Use Python 3.11+, `readelf` e `aapt` para obter também versões de manifestos binários. Substitua os caminhos; informe somente os APKs reais do conjunto, omitindo o segundo se o jogo for um APK único:

```sh
python3 tools/inventory_apk.py /private/owner-input/base.apk   /private/owner-input/config.arm64_v8a.apk   --output work/inventory.json
python3 tools/find_reference.py --engine Unity --abi arm64-v8a --renderer GLES2
```

O SDK público inclui as ferramentas. Para acessá-las no container, monte o input explicitamente como somente leitura em `/private/owner-input`, além da coleção e `work/`. Não envie os inputs a CI ou serviços externos. O arquivo de saída deve ser novo.

## 2. Interpretar os campos

O relatório traz package/split, versão quando disponível, tamanho/hash do container, bibliotecas por ABI, hashes, imports com tipo/binding/versão, dependências e indícios de engine. Versão ausente fica `null`; engine não identificada permanece hipótese. O catálogo tem 40 perfis de fontes, com campos desconhecidos explícitos e links de evidência. ABI significa declaração da receita histórica, não execução validada nesta edição.

A preferência por `arm64-v8a` é local ao APK que contém essa ABI. Examine todo o conjunto de splits; um split de assets não define a arquitetura. Builds ARMv7 exigem a revisão da fronteira softfp.

## 3. Respeitar os limites

O inventário rejeita caminhos inseguros, colisões, symlinks, membros criptografados, membros acima de 512 MiB e conjuntos acima de 8 GiB. APKM/APKS/XAPK aninhados não são expandidos automaticamente: prepare o conjunto localmente em área privada. A ferramenta não garante completude de splits e não converte arquivos.

TLS, TLSDESC, IFUNC, IRELATIVE, RELR e relocações Android compactadas exigem atenção antes de escolher o nxloader V5. Os marcadores são uma triagem estática, não uma certificação completa de todas as relocações. Imports via dlsym/JNI e contratos de buffers/threads ainda precisam ser investigados.

## 4. Entregar uma próxima ação à IA

Selecione uma referência pelo perfil, abra `SOURCE-MAP.json` e leia a implementação que corresponde à fronteira encontrada. Guarde assinatura, tipo, ownership, erro e teste de cada import em sua tabela de contratos. O [exemplo integrado](../../examples/first-port/README.md) fornece um primeiro input autoral para praticar sem dados comerciais.
