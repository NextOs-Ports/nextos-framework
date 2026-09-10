# Ferramentas de memória e diagnóstico

Estas ferramentas existem para que desenvolvimento, teste e recuperação não dependam da
memória de uma sessão interativa.

## `run-logged.sh`

Executa um comando em primeiro plano e cria um diretório novo, sem sobrescrever execuções
anteriores. Antes de iniciar o comando, grava horário UTC, diretório, PID, Git HEAD,
quantidade de argumentos, SHA-256 do vetor de argumentos exato e uma representação escapada
com opções conhecidas de credencial redigidas. Arquivos regulares explícitos são hasheados,
exceto valores e paths passados por opções de segredo. Depois preserva stdout/stderr, status
final real do comando, status do `tee`, sinal observado pelo runner e um manifesto SHA-256.

O comando roda em uma única pipeline assíncrona pertencente ao próprio Bash. O status do
comando é gravado separadamente em `command-status.txt`; assim ele não é confundido com o
status de `tee`. Se um sinal tratado interromper `wait`, o runner consulta apenas sua própria
tabela de jobs (`jobs -pr`) e volta a esperar aquele mesmo filho. Ele não procura processos
em `/proc`, não usa `kill -0` e não adota um PID externo. `command-status.txt` também entra
no `MANIFEST.sha256`, junto com metadados, console, resultado e hashes de entrada.

Os handlers são armados antes de `metadata.txt` ser publicado. Depois que a pipeline foi
integralmente coletada, HUP/INT/TERM passam a ser ignorados até o `exit`: esse é o cutoff
explícito que congela `received_signal`, `result.txt`, `console.log` e o manifesto como um
único recibo coerente. Um `KILL` continua incapturável e, por definição, deixa uma execução
parcial sem manifesto válido.

O runner não enumera processos, não envia sinais, não inicia serviços e não altera sessão,
energia ou frontend. Ele também não despeja o ambiente inteiro, evitando registrar
credenciais por acidente.

A redação protege os metadados criados pelo runner. `console.log` é a saída literal do
comando filho; portanto, um gate não pode imprimir segredo em stdout/stderr e deve usar uma
fixture silenciosa para testar argumentos sensíveis. O digest do vetor exato permite provar
qual invocação foi feita sem persistir seu conteúdo secreto.

Exemplo:

```sh
framework/tools/run-logged.sh \
  --log-root /caminho/absoluto/para/logs -- \
  bash -n framework/nxbootstrap/nxbootstrap.sh
```

## `capture-checkpoint.sh`

Cria um checkpoint append-only fora do repositório. O checkpoint contém metadados Git,
status dos caminhos escolhidos, patches tracked, hashes de todos os arquivos e um tarball
dos caminhos — incluindo arquivos ainda untracked. Não inclui `.git` e nunca remove um
checkpoint anterior.

```sh
framework/tools/capture-checkpoint.sh \
  --repo /caminho/absoluto/do/repo \
  --checkpoint-root /caminho/absoluto/para/checkpoints -- \
  framework suportando_outros_devices
```

Os testes que exercitam sinais/processos continuam proibidos no host. Log não é isolamento:
esses testes só rodam pelo gate canônico depois de comprovar user/PID/mount namespaces
privados, `/proc` privado e PID interno 1.
