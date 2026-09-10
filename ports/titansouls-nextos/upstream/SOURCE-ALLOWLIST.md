# Source-only repository boundary / Limite do repositório de fontes

This repository is an explicit source allowlist. It contains only the
Titan Souls compatibility adapter, its tests, data-free extraction recipe,
release metadata, documentation, official promotional screenshots used by the
README and port-specific build/package recipes.

It does **not** contain copied NextOS framework source, generated framework
runtime files, NXExtract runtime or UI, compiled executables or libraries,
APK/OBB content, extracted game assets, saves, logs or support bundles.

The three files under `docs/images/` are unmodified official Titan Souls store
media used only to identify and illustrate the game. They are not part of the
port, build inputs or release ZIP, and remain property of Acid Nerve / Devolver
Digital.

The test release asset is assembled outside the Git source tree from pinned
external framework/NXExtract inputs. Owner-provided Android data is never part
of either the source repository or the release asset.

---

Este repositório usa uma allowlist explícita. Ele contém somente o adapter de
compatibilidade de Titan Souls, testes, receita de extração sem dados,
metadados de release, documentação, imagens promocionais oficiais usadas no
README e receitas de build/pacote específicas do port.

Ele **não** contém cópias das fontes do framework NextOS, arquivos gerados do
framework, runtime/UI do NXExtract, executáveis ou bibliotecas compiladas,
conteúdo de APK/OBB, assets extraídos, saves, logs ou bundles de suporte.

Os três arquivos em `docs/images/` são mídias oficiais da loja de Titan Souls,
sem modificações, usadas apenas para identificar e ilustrar o jogo. Não entram
no port, no build nem no ZIP e continuam pertencendo à Acid Nerve / Devolver
Digital.

O asset de release de teste é montado fora da árvore Git a partir de entradas
externas pinadas do framework/NXExtract. Dados Android fornecidos pelo dono não
entram no repositório nem no asset de release.
