# nxbootstrap execution roles v1

`execution_roles` é uma extensão opt-in do manifesto v2 introduzida no
nxbootstrap 0.6.27. A ausência do campo preserva a rota histórica. Sua presença
exige os quatro membros `extractor`, `splash`, `game` e `helpers`.

Cada papel ELF declara exatamente:

- `architecture`: `aarch64`, `armv7`, `x86_64` ou `i386`;
- `executable`: caminho relativo e contido no port;
- `executor`: `native` ou `native-or-loader`;
- `interpreter`: `PT_INTERP` Linux canônico da arquitetura;
- `closure`: `host`, `firmware` ou `firmware-and-port`.

O papel `extractor` descreve o executável ELF sensível à ABI da fase NXExtract,
isto é, `nxextract-ui`; scripts Bash/Python permanecem neutros. Ele usa execução
nativa e closure host. Quando NXExtract está desativado, esse papel é `null`.
O splash usa `nxsplash-nextos` e closure firmware. O jogo precisa repetir a
arquitetura/executável históricos e usa closure `firmware-and-port`. Helpers
adicionais acrescentam `id`, são ordenados canonicamente e integram
`required_files`.

O resolvedor valida magic ELF, classe, little-endian, `e_machine` e
`PT_INTERP`. Se o interpreter canônico existe, a execução é nativa. Somente um
papel `native-or-loader` pode procurar alternativa; cada candidato também passa
pela validação ELF completa. Nenhum caminho alternativo é escolhido por nome de
aparelho ou CFW.

Ao montar o closure, cada arquivo sondado numa raiz recebe uma de três
respostas, e elas não são intercambiáveis:

| Resultado | Ação |
|---|---|
| biblioteca de runtime da ABI esperada | a raiz entra no closure |
| ELF de runtime com ABI errada | a raiz **inteira** é recusada (`reason=wrong-abi`) |
| arquivo que não é ELF | é **ignorado** (`reason=not-an-elf`) e não conta como raiz vista |

A recusa por ABI errada é a proteção real do contrato e não foi afrouxada. O
terceiro caso existe porque distribuições multiarch entregam `libc.so` como
**script do GNU ld** ao lado da `libc.so.6` de runtime: o script é artefato de
build. Tratá-lo como "ELF inválido" descartava a raiz correta e encerrava o
preflight antes de o instalador desenhar qualquer coisa. Uma raiz cujo único
casamento seja um script continua **não** virando closure.

Closures usam um catálogo finito de candidatos e só incorporam raízes existentes
com providers da ABI esperada. No ARMHF do Spruce, as raízes comprovadas do
chroot e de `usr/lib32` são candidatas, enquanto o `muOS/usr/lib` AArch64 fica
explicitamente fora. O manifesto não fixa caminhos absolutos do cartão. Roots
privados do jogo entram somente depois da extração e são revalidados antes do
runtime.

Cada resolução emite `EXECUTION RECEIPT` com papel, ABI, executor efetivo,
interpreter, loader, closure, roots e motivo. Ausência de executável, identidade
ELF divergente, interpreter inesperado, loader inválido ou closure vazio encerra
o preflight com falha. O contrato não altera SDL, EGL/GLES, NXSplash, NXExtract
visual nem o lifecycle nativo do jogo.
