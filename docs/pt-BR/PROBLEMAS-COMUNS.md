# Problemas comuns e próximo diagnóstico

[English](../en/TROUBLESHOOTING.md)

Guarde o comando, a primeira falha significativa, a ABI e as versões das ferramentas. Não substitua o diagnóstico por várias mudanças globais de flags. Execute scripts das referências somente depois de ler seus caminhos e efeitos.

| Sintoma | Conferir primeiro | Próximo passo |
| --- | --- | --- |
| Clone privado recusado | Permissão da conta e autenticação GitHub | Corrigir acesso sem escrever token no comando |
| CMake não encontra compilador | Caminho executável e toolchain carregado | Usar diretório de build novo ao trocar compilador |
| `crt1.o`/`crti.o` ou `-lc` ausente | Sysroot de desenvolvimento completo e ABI | Corrigir SDK; não linkar arquivos x86 no build ARM |
| “file in wrong format” | `readelf -h` em objetos e bibliotecas | Separar Linux/Android, ARM32/ARM64 e host |
| “Exec format error” | Arquitetura do executável versus host | Executar no alvo correto ou emulação explícita |
| Nenhum teste CTest no cross | `CMAKE_CROSSCOMPILING` e CMakeLists do exemplo | Registrar build concluído; teste ARM ainda pendente |
| `GLIBC_x.y not found` | Versões requeridas por todos os ELFs | Recriar apenas o candidato novo com sysroot compatível |
| Import resolvido seguido de crash | Tipo, assinatura, layout, TLS e ownership | Teste dirigido do contrato, não cast genérico |
| JNI retorna null e trava depois | Classe/método/campo e exceção esperada | Implementar objeto/callback real requerido |
| Unity chega ao terceiro frame e para | Present, sincronização e callbacks | Medir a fronteira; patch Swappy só no perfil comprovado |
| Áudio com tela preta | Pixels pré-present, FBO, shader e compositor | Invalidar vídeo; consultar diagnóstico Unity/Godot |
| Texto em blocos/quadrados | Canal do atlas, alpha, SDF e sampler | Corrigir o formato/material específico |
| Áudio rápido/lento | Taxa real do mixer versus dispositivo | Ajustar contrato ou conversão, preservando contagem de frames |
| Botão confirma e cancela | Eventos duplicados e duas autoridades | Uma rota por ação/contexto com release real |
| Clique fora do botão | Drawable, viewport e origem Y | Transformar posição e delta no espaço do consumidor |
| NXExtract só aceita pasta pronta | Receita, input completo e hooks | Testar do zero; adoção não prova instalação |
| Launcher retorna antes de log | Log de erro inicial e paths resolvidos | Diagnosticar a fronteira anterior ao runtime |

## Informar um problema de forma útil

Crie um relato com commit da coleção/port, SHA do executável, sistema/ABI/GPU, perfil técnico do jogo, comando sem segredos, resultado esperado, observado e teste já feito. Diga se o erro é host, emulação ou aparelho. Não anexe dados comerciais, dumps ou logs privados completos.

Para uma regressão, compare com o executável aprovado e a mesma cópia de dados. Preserve esse artefato e a receita antes de reconstruir. Uma mudança de dados pode explicar uma diferença mesmo com o mesmo nome de jogo.

Leia [shims](SHIMS.md), [Unity](../../portando_unity/README.md), [Mono Android](MONO-ANDROID.md), [Godot](GODOT.md) e [Cocos2d-x](COCOS2D-X.md) conforme a engine.
