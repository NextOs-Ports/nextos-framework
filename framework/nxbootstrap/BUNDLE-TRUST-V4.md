# nxbundle-v1 — raiz de confiança e modelo de ameaça (V4-REPACK-01)

Este documento é escrito **antes** do código que ele governa e é a única
autoridade sobre o que a autenticação do seed promete e o que ela não promete.

## 1. O problema de produto

Na V3 a closure autenticada só existia dentro de `<port-id>/.nxruntime/
generations/<id>/`. Um ZIP pessoal — extrair o ZIP oficial, adicionar dados
legais em `gamedata/`, recompactar os dois itens da raiz — é uma operação
comum, e muitos compactadores gráficos omitem dotdirs. O resultado era a recusa
`NXU0002`/`NXU0009`: tecnicamente correta, mas exigindo que uma árvore oculta
crítica sobrevivesse a uma ferramenta que não a enxerga.

A V4 não relaxa o selo. Ela move a autoridade para um arquivo **regular e
visível**.

## 2. Formato

```text
<port-id>/nxruntime-<generation-id>.nxb        modo 0644, arquivo regular
```

```text
NXBUNDLE1
generation <generation-id>
port <port-id>
members <n>
M<TAB>0644|0755<TAB><sha256><TAB><bytes><TAB><offset><TAB><path>   (n linhas)
END
<payloads concatenados, na ordem do cabeçalho>
```

Determinístico: ordem canônica, sem timestamps, sem UID/GID, sem atributos do
transporte. Regerar o mesmo port com as mesmas entradas produz os mesmos bytes.
O nome é content-addressed pelo id da geração.

Os `path` são relativos à raiz da geração e cobrem `format`, `manifest.json`,
`components.sha256`, `components.v2`, `identity.json`, `identity-runtime.v2` e
tudo sob `files/`. **`commit` nunca é transportado**: ele é escrito no
aparelho, por último, como recibo local de que a closure chegou completa.

## 3. Raiz de confiança

A raiz é **o id de geração compilado no launcher** (`@GENERATION_ID@`). O
launcher é o item que o HarbourMaster precisa achar na raiz do ZIP para que
qualquer código nosso rode; ele sempre sobrevive ao rezip.

A cadeia, sem nenhum passo circular:

1. `sha256(identity.json)` **tem de ser igual** ao id de geração compilado.
   Isso é verificado pelo mesmo validador fechado já usado por uma geração
   instalada (`nxbootstrap_generation_v2_authenticated`).
2. `identity.json` fixa `nxport_sha256`, `runtime_records_sha256`,
   `launcher_preimage_sha256` e o registro `role/mode/sha256/path` de **todo**
   membro de runtime.
3. `components.v2` e `components.sha256` são conferidos contra esses registros,
   e cada arquivo materializado é conferido contra o seu SHA-256.
4. O `files/launcher/<nome>` do seed é comparado ao SHA-256 do **launcher que
   está executando**. Um launcher que não pertence àquela closure é recusado.
5. `files_closed` garante que não existe um único arquivo a mais dentro da
   geração materializada.

O launcher **não** carrega o hash do próprio seed, e o seed **carrega** os
bytes do launcher: por isso a cadeia fecha em vez de girar. Self-hash circular
não seria autenticação e não é usado aqui.

## 4. O que o modelo detecta

- seed ausente, truncado ou corrompido;
- qualquer byte adulterado de qualquer membro;
- cabeçalho reescrito de forma consistente com um payload adulterado
  (`identity.json` deixa de bater com o id compilado);
- seed de outra geração colocado no lugar;
- membro extra, membro faltando, contagem errada, offset deslocado;
- path traversal, path absoluto, duplicata, modo fora de `0644`/`0755`;
- symlink, FIFO ou diretório no lugar do seed;
- launcher instalado que não pertence à closure do seed;
- staging interrompido por queda de energia (nunca é adotado nem "curado").

## 5. O que o modelo NÃO promete

Não há resistência contra **um administrador local do próprio aparelho**. Quem
pode escrever no cartão pode substituir launcher e seed em conjunto, ou trocar
o próprio verificador. Nenhum esquema puramente local resolve isso, e afirmar
o contrário seria mentira de segurança.

O que o modelo garante é integridade e procedência **dentro da instalação**:
qualquer divergência entre o que o autor publicou e o que está no cartão é
detectada e falha fechada antes do NXExtract e antes do jogo.

O ZIP pessoal muda de SHA-256 externo — isso é esperado e legítimo. A
identidade verificável continua sendo a do bundle interno, não a do ZIP.

## 6. Fronteira selada × fronteira do dono

| Selado (identidade do runtime) | Do dono (fora da identidade) |
|---|---|
| launcher, `nxport.json`, seed `.nxb`, executável, bibliotecas privadas, hooks, NXExtract, receita, NXSplash | `gamedata/`, saves, `NEXTOSCONTROLLERS.gptk`, `NEXTOSSETTINGS.txt`, logs, `.nxruntime` como cache |

Acrescentar arquivos regulares em `gamedata/` **não** recalcula a geração e
**não** invalida o runtime. Dados errados falham no gate de dados/NXExtract,
nunca como ausência de runtime.

## 7. `.nxruntime` é cache

Apagar `.nxruntime` inteiro é recuperável: a próxima abertura reconstrói a
geração somente a partir do seed autenticado. Uma geração já existente —
completa, truncada ou adulterada — **nunca** é reparada no lugar e um `commit`
ausente **nunca** é fabricado por cura. O caminho de reconstrução só roda
quando o diretório da própria geração está ausente.

## 7b. Custo de leitura

O cabeçalho é relido com `head` limitado, nunca com `sed` sobre o arquivo
inteiro: o seed carrega todo o payload do port e reparsear o cabeçalho lendo
tudo custaria centenas de MB do cartão a cada reconstrução (medido: 91 ms
contra 1 ms num seed de 300 MB, já em cache).

A extração de cada membro usa `tail -c +offset | head -c size`. Isso depende de
`tail` fazer `lseek` num arquivo regular, o que o coreutils faz — medido em
200 extrações espalhadas por 200 MB em 128 ms, ou seja, sem releitura
quadrática. Numa CFW cujo `tail` não faça seek o custo cresceria; isso é uma
propriedade conhecida e registrada, não uma medição de firmware que não temos.

## 7c. Colisão de caixa: a mídia do aparelho é insensível

`/roms` é exFAT. Dois membros que diferem só na caixa — `files/lib/Game.so` e
`files/lib/game.so` — são **distintos no cabeçalho e o mesmo arquivo no
cartão**: um sobrescreve o outro em silêncio e a closure que acaba verificada
não é a closure declarada. Antes isso só aparecia depois, como divergência de
hash num membro aparentemente inocente, ou não aparecia de jeito nenhum quando
os dois membros tinham o mesmo conteúdo.

O cabeçalho agora recusa a ambiguidade onde ela ainda pode ser **nomeada**, com
uma única passagem (`tr` + `sort` + `uniq -d`) sobre a lista de caminhos já
validados — nada de comparação O(n²) por membro. A recusa cobre também a
duplicata exata, que passa a ter diagnóstico próprio em vez de virar
`NXU0009: closure is absent`.

A mesma recusa existe no gerador, e as duas são necessárias: a do launcher
protege contra seed adulterado, a do gerador impede que um seed nosso saia
assim de fábrica.


## 7d. A linha que o instalador possui

O PortMaster **reescreve a linha 2** de um launcher instalado, trocando-a pelo
nome do zip de onde ele veio — é assim que o harbourmaster sabe de que arquivo
um launcher saiu. Medido no aparelho autorizado com um `harbourmaster install`
de verdade: exatamente esses bytes mudam, e mais nada.

A âncora original comparava o launcher instalado **byte a byte** com a cópia da
closure. Isso funcionava quando eu instalava o ZIP à mão — e recusava o port no
único caminho que os usuários realmente usam:

```
UPDATE NXU0012: installed launcher does not match the runtime seed closure
UPDATE NXU0009: generation-v2 closure is absent; launch refused
```

Reivindicar uma linha que o instalador possui é defeito nosso, não do
PortMaster. A comparação **instalado × closure** passa a ser feita sobre a
forma canônica: aquela única linha de comentário normalizada nos dois lados,
todo o resto — inclusive o id de geração compilado e todo o código — ainda
ancorado exatamente. A linha só é normalizada quando ela realmente é o
marcador; um launcher cuja linha 2 seja outra coisa é comparado como está.

**Nenhum hash gravado enfraquece.** O manifesto não muda e a integridade da
closure continua verificada byte a byte contra `components.v2`; o que muda é
só a relação entre o que o instalador escreveu e o que a closure carrega.

## 8. Espaço

Antes de materializar, o launcher publica um recibo
`STORAGE: bundle=… active=… pending=… previous=… required=… available=…` e
recusa com `NXU0013` quando o espaço disponível não cobre o custo total mais a
margem. Nenhuma geração ativa é destruída para abrir espaço.
