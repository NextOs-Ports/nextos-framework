# Testes, evidência e entrega

[English](../en/TESTING.md)

Um resultado útil diz quais bytes foram testados, com quais dados e em qual ambiente. Este guia separa os testes necessários para desenvolver uma referência dos testes que sustentam uma release de jogo.

## Durante o desenvolvimento

Execute testes pequenos ligados à mudança: contrato de um shim, parser de receita, tradução de estrutura, sequência de input ou preparação de dados sintéticos. Não rode a suíte completa do framework a cada ajuste. Não refaça um build aprovado apenas para traduzir o README.

Registre falhas como hipóteses testáveis. Uma contraprova deve mostrar por que o reparo não se aplica a outro caso: por exemplo, preservar um sampler válido ao corrigir um formato de textura específico.

## Matriz mínima por port

| Etapa | O que demonstra | O que não demonstra |
| --- | --- | --- |
| Verificação de fontes/hashes | Origem e integridade da seleção | Licença integral ou gameplay |
| Teste host | Contratos exercitados no computador | ABI Android completa ou GPU do aparelho |
| Build ARM e auditoria ELF | Arquitetura e dependências declaradas | Execução física |
| Emulação de CPU | Caminhos executados naquele ambiente | Driver Mali, áudio ou input físico |
| Primeiro frame real | Imagem daquele ponto e binário | Progressão, save ou todas as cenas |
| Teste de gameplay | Ações/cenas explicitamente percorridas | Campanha inteira sem percurso registrado |
| NXExtract do zero | Preparação do input completo com aquele ZIP | Outro APK, receita ou pacote |

## Prova física

Só use o aparelho autorizado na tarefa. Antes de abrir, confirme que não há outra instância do mesmo jogo, incluindo executável substituído com processo antigo ainda vivo. Não coloque arquivos de port numa pasta reservada à atualização do firmware.

Meça o contexto/drawable real e pixels imediatamente antes do present quando instrumentável. Áudio vivo, PID, contexto criado ou `exit 0` não provam imagem. Preto conclusivo ou contexto morto invalida o teste; preserve o diagnóstico e encerre somente a instância exata.

Exercite início, menu, gameplay, transições, pausa, áudio, controles/contextos, hotplug, save/reload e saída. Relate cenas e duração realmente observadas. Uma observação do dono complementa os dados do teste, sempre vinculada ao artefato correto.

## Congelar antes do pacote

Conclua fonte, receita, documentação bilíngue, licença, pins, build e auditorias. Preserve o hash do executável fisicamente aprovado num registro externo ao port; relink ou rebuild exige nova prova. Não renomeie um ZIP antigo como se fosse versão nova.

O empacotador canônico do port deve coordenar uma bateria final: validar manifestos/fontes, preparar stage e verificar stage **antes** de criar o único ZIP candidato daquele commit. Não execute essas fronteiras completas separadamente e depois as repita no empacotador. Se surgir falha, reúna os erros, corrija em conjunto e congele um novo commit antes da tentativa seguinte.

## Conteúdo da release

Confira framework completo, NXExtract gráfico e receita real, NXSplash de cinco segundos, launcher gerado, manifesto verdadeiro, `<port-id>/INSTALLATION.md` em PT/EN, créditos/licenças e fontes correspondentes exigidas. Audite todos os ELFs Linux para GLIBC ≤ 2.30. Use SDL do sistema por padrão e rejeite redirecionamento acidental a SDL privada.

Audite scripts executáveis sem dependência do comando externo `stat`; ler `/proc/<pid>/stat` é diferente. Faça os testes prévios de falha de launcher antes do log normal. Nenhum APK, asset, biblioteca original ou dado preparado de jogo entra no ZIP.

## Aceitar o candidato exato

Após criar o candidato, teste [instalação limpa pelo NXExtract](NXEXTRACT.md) a partir do input completo e preserve o ZIP sem regenerar seus bytes. Vincule os receipts, receita e hashes das saídas ao mesmo ZIP e executável. Adoção de dados antigos não basta. Registre os dispositivos/firmwares testados e os ainda não testados.

A coleção atual tem verificação de fontes e exemplos host; não passou por uma nova bateria de 44 jogos. Consulte [validação desta publicação](../../publication/VALIDATION.md). Publicar a coleção como pública continua dependendo da aprovação de NextOS.
