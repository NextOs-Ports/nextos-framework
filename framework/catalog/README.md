# Catálogo de evidências do framework

Este diretório transforma o histórico dos ports em dados verificáveis. Ele não é uma
lista de jogos que “devem funcionar”: cada entrada separa publicação, escopo realmente
validado, evidência física, correções reaproveitáveis e limites que não podem virar regra.

Arquivos:

- `ports-v1.json`: inventário estruturado e decisões por referência;
- `port-check-definitions-v1.json`: matriz fixa de 42 checks por entrada;
- `generate-port-checks.py`: expansão determinística `PORT-{port}-{check}`;
- `port-checks-v1.tsv`: expansão versionada gerada dos dois JSON acima;
- `abi-variants-v1.json`: ABIs explicitamente aplicáveis, sem inferência por texto;
- `abi-check-definitions-v1.json`: matriz fixa de 28 decisões por ABI;
- `generate-abi-checks.py`: expansão `ABI-{port}-{armv7|aarch64}-{check}`;
- `abi-checks-v1.tsv`: expansão ABI versionada;
- `local-port-directories-v1.txt`: snapshot nominal dos diretórios locais, sem afirmar
  que são completos, públicos ou aprovados.

Para regenerar a expansão:

```sh
python3 framework/catalog/generate-port-checks.py \
  --catalog framework/catalog/ports-v1.json \
  --checks framework/catalog/port-check-definitions-v1.json \
  --format tsv \
  --output framework/catalog/port-checks-v1.tsv

python3 framework/catalog/generate-abi-checks.py \
  --variants framework/catalog/abi-variants-v1.json \
  --checks framework/catalog/abi-check-definitions-v1.json \
  --output framework/catalog/abi-checks-v1.tsv
```

Os geradores recusam IDs duplicados, quantidades diferentes de 42/28 checks e campos
ausentes. A expansão ABI contém duas referências ARMv7 positivas e atuais (KOTOR e
TASM2 1.2.7d) e três referências AArch64 positivas (Bully2 em escopo histórico e
estreito, Sonic 4 Episode II v6 e os três módulos de Horizon Chase). Cada variante
mantém exatamente 28 decisões; tentativas ARM antigas, WIPs e semelhança textual não
criam uma ABI. Guests AArch64 não recebem `PT_INTERP`; somente o host Linux usa
`/lib/ld-linux-aarch64.so.1`. Evidência ABI não autoriza copiar hooks, offsets ou
atalhos de lifecycle dos jogos.
Um valor como `not-applicable: ...` é uma decisão explícita; ausência de campo é erro.
Fontes aprovadas que vivem em outro repositório usam o locator portátil
`external:<repositorio>` junto do URL e commit fixos; o catálogo nunca publica um
caminho pessoal da máquina que produziu o inventário.
