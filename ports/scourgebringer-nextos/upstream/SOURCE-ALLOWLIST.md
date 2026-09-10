# Source-only repository boundary / Limite do repositório de fontes

This repository contains only ScourgeBringer-specific adapter source, host
tests, the data-free extraction recipe, immutable build/release metadata,
documentation, release notes, screenshots and port-specific build/package
recipes.

It deliberately does **not** track the shared NextOS framework, a generated
nxbootstrap launcher, the NXExtract runtime/UI, compiled Linux executables or
libraries, an APK/OBB, extracted Android libraries or assets, saves, logs or
support bundles. The release recipe materializes the exact pinned shared
inputs in a temporary source mirror outside the Git work tree and verifies
their SHA-256 values before packaging, then removes those temporary copies.

`package/README.md` is the frozen, audited README placed inside test.8. The
NXExtract MIT notice is kept because every distributed ZIP containing
NXExtract must preserve its licence. The four PNG files under `screenshots/`
document physical validation and are not build inputs or release contents.

Owner-provided Android data is never part of the source repository or the
data-free release ZIP.

---

Este repositório contém somente o código do adapter específico de
ScourgeBringer, testes host, receita de extração sem dados, metadados imutáveis
de build/release, documentação, notas de versão, capturas e receitas de
compilação/empacotamento específicas do port.

Ele deliberadamente **não** rastreia o framework compartilhado do NextOS,
launcher gerado pelo nxbootstrap, runtime/UI do NXExtract, executáveis ou
bibliotecas Linux compilados, APK/OBB, bibliotecas ou assets Android extraídos,
saves, logs ou bundles de suporte. A receita de release materializa as entradas
compartilhadas exatas e fixadas num espelho temporário fora da árvore de trabalho
do Git, confere os respectivos SHA-256 antes de empacotar e remove essas cópias
ao fim.

`package/README.md` é o README congelado e auditado que entra na test.8. O
aviso MIT do NXExtract permanece porque todo ZIP que distribui o NXExtract deve
preservar sua licença. Os quatro PNGs em `screenshots/` documentam a validação
física e não entram no build nem no ZIP.

Dados Android fornecidos pelo dono nunca entram no repositório nem no ZIP sem
dados proprietários.
