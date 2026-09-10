# Shims e referências reutilizáveis

Um shim traduz um contrato entre o código Android e seu host Linux. Resolver o nome de uma função é apenas a primeira etapa: assinatura, ABI, layout, ownership, erros e callbacks precisam estar corretos.

O [exemplo compilável](../../examples/shims-reference/README.md) ensina resolução tipada, erro explícito, propriedade conhecida e errno por thread. Ele é pequeno por intenção e não é uma camada Android completa. A coleção real é a base para a IA encontrar implementações maiores.

| Necessidade | Onde começar a leitura |
| --- | --- |
| ELF, relocações, imports e ordem dos construtores | `framework/nxloader/` |
| Lifecycle e fronteiras Android | `framework/nxandroid/` |
| Janela, contexto, drawable e present | `framework/nxgl/` |
| Áudio e observabilidade | `framework/nxaudio/` e `framework/nxobs/` |
| Controles e contextos | `framework/nxinput/` |
| Compatibilidade JNI/Unity | fontes de Bomb Chicken/Gunbrick, Suzy Cube, Horizon Chase, Nameless Cat e demais Unity do catálogo |
| NativeActivity e APIs nativas | fontes de Castle of Illusion, Sonic 4 Episode II, KOTOR e outras referências nativas |
| Cocos2d-x | Chrono Trigger e Geometry Dash/SubZero |
| MonoGame/.NET | SOR4, Stardew Valley, ScourgeBringer e Blossom Tales |
| GameMaker | Forager, preservando a licença GPL-2.0-only e seu runtime independente |

Os diretórios acima indicam pontos de investigação, não autorização para promover todos os seus comportamentos ao núcleo. Leia o manifesto individual e o resultado conhecido antes de copiar uma função.

## Meta da base ampliada

Cobrir contratos recorrentes de Bionic, pthread/TLS, JNI/objetos/strings, assets, looper, native window, OpenSL ES/AudioTrack, EGL/GLES e entrada. Separar perfis de engine e ABI. Marcar cada contrato como implementado/testado, dependente de adapter, opcional explicitamente inerte ou não suportado.

A IA deve recusar imports obrigatórios desconhecidos e produzir a lista exata dos contratos faltantes. Não trocar isso por `return 0`, objetos fictícios ou mutex sempre recursivo. Stubs opcionais só são aceitos quando o comportamento inerte for correto e demonstrado naquele contrato.

O objetivo é diminuir adaptações novas nos jogos de famílias já conhecidas. A cobertura deve ser medida nos títulos Android do catálogo. Esta primeira entrega não promete compatibilidade com a maioria de todos os jogos Android.
