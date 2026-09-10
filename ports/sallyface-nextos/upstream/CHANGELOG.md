# Changelog — Sally Face

## sallyface-universal-1.1.4 — 2026-08-28

- Migração da pilha de empacotamento para a release **final** `framework-v3`
  (`nxbootstrap-v0.6.37`, `nxgenerator-v0.2.20`, `nxrelease-v0.2.43`), saindo do
  candidato `framework-v3-rc5-20260828`. Launcher regerado por essa pilha.
- O loader do jogo **não mudou**: o `FRAMEWORK-BUILD-PIN` de `nxinput`, `nxgl` e
  `nxcompat` é o mesmo da 1.1.3 e o binário sai byte a byte idêntico
  (`73a74ede15bdbe07ee84b2666da234e9244e24594f8e6011b174a7b311bfbfc3`).
- Sem mudança de comportamento no aparelho: mesma extração, mesmos controles,
  mesma imagem e mesmo áudio da 1.1.3.

## sallyface-universal-1.1.3 — 2026-08-28

- Corrigido o retag GLES2 de instalação nova: os perfis internos convertiam os
  objetos `Shader`, mas omitiam o nó `globalgamemanagers`. Por isso a árvore
  publicada ainda declarava Vulkan/GLES3 em `BuildSettings` e GLES3 mínimo em
  `PlayerSettings`, fazendo a Unity usar o `Hidden/InternalErrorShader` rosa.
- Os dois perfis autenticados agora convertem também `m_GraphicsAPIs` para
  `[8]` e `playerMinOpenGLESVersion` para `1`, com SHA-256 novo do nó e do
  `data.unity3d` reconstruído. Os nove assets de cena continuam idênticos aos
  resultados anteriormente provados.
- O gerador do delta passou a tratar settings e shaders pelo mesmo caminho
  verificado por objeto. O gate pós-retag exige `[8]`, mínimo `1`, 44 shaders e
  ausência de plataforma 9 ou tipo de programa 4, inclusive dentro dos blobs.
- Preservados os aliases ESSL da 1.1.2, ABI AArch64, saves, controles, áudio,
  qualidade, interfaces e pins imutáveis do framework RC5.
- Receita elevada para `arm64-4` com recibo transacional persistente. Dados
  preparados pela `arm64-3` não podem ser adotados silenciosamente: o
  NXExtract refaz o preparo a partir dos dados legítimos do dono para aplicar
  o contrato gráfico completo, sem tocar nos saves.

## sallyface-universal-1.1.2 — 2026-08-28

- Corrigido o fallback raw EGL do Unity 2022.3 que ainda definia os aliases
  `ATTRIBUTE_IN`, `VARYING_IN`, `VARYING_OUT`, `DECLARE_FRAG_COLOR`,
  `FRAG_COLOR` e `SAMPLE_TEXTURE_2D` para ESSL300 depois do retag GLES2.
  Os aliases agora são convertidos para o conjunto ESSL100 nativo da própria
  Unity; `DECLARE_FRAG_COLOR` é removido para preservar sua guarda `#ifdef`.
- A tradução exige o bloco completo e coerente, valida que nenhum token moderno
  permaneceu e emite recibos separados para vertex e fragment. Blocos parciais
  falham fechados em vez de produzir fonte híbrida.
- Adicionadas regressões com os dois shaders reais e validação sintática
  `glslangValidator`. ESSL100 já válido permanece byte a byte, preservando os
  caminhos anteriormente funcionais, dados extraídos, saves e controles.
- Mantidos sem alteração a receita `arm64-3`, ABI AArch64, NXExtract 1.2.21,
  NXSplash, qualidade e os pins imutáveis do framework RC5 usados pela 1.1.1.

## sallyface-universal-1.1.1 — 2026-08-28

- Corrigido o fallback raw EGL no qual a Unity recompilava fonte marcada como
  ESSL100 ainda contendo `in`/`out` de GLES3, causando falha de link, GL
  `0x502` e tela preta. ESSL100 legítimo continua intocado.
- O diretório de saves agora é criado antes do recibo de ABI do shader, sem
  alterar saves ou caches que já existam.
- Migração imutável para `framework-v3-rc5-20260828`,
  `nxgenerator-v0.2.18` e `nxrelease-v0.2.39`; documentação pública entra como
  payload authored e a closure atômica NXExtract/runtime é auditada pelo RC5.
- Mantidos o guard de memória apenas observador e os quatro perfis oficiais de
  qualidade introduzidos em 1.1.0.

## sallyface-universal-1.1.0 — 2026-08-28

- Removido o encerramento artificial `_exit(70)`: o guard de RAM agora apenas
  observa e registra pressão, inclusive abaixo de 60 MB disponíveis.
- `quality=auto|low|medium|high` passou a usar o parser canônico fail-closed,
  presets oficiais Low/High da Unity e política seletiva de textura/cache.
- `auto` escolhe pela RAM instalada; o perfil low foi medido no aparelho de
  baixa memória e todos os perfis emitem recibos de resolve/apply/runtime/ready.
- Adicionado perfil interno seguro para a cópia de referência 1.5.53 sem
  prender aceitação ao nome, assinatura, versão ou SHA integral do APK.
- Opt-in imutável em `framework-v3-rc3-20260828`, `nxbootstrap-v0.6.36`,
  `nxgenerator-v0.2.16` e `nxrelease-v0.2.37`, com runtime v2 e closure NXExtract
  completa (receita, engine, runner, ambiente, UI, helpers e spec).
- Fullscreen 4:3, GLES, áudio, cursor, GPTK, troca A/B, SELECT+START, saves e
  fluxo Android/Unity permanecem inalterados.

## sallyface-universal-1.0.0 — 2026-08-27

- Tela 4:3 preenchida pelo presenter final GLES2: o retângulo central 16:9 já
  composto pelo Adventure Creator é copiado e escalado uma única vez antes do
  swap. Em 16:9 a rota permanece identidade; `SF_ASPECT=native` é rollback.
- Removida a tentativa ineficaz de interceptar viewport/scissor. O estado GL
  do jogo é preservado durante a apresentação final.
- `NEXTOSCONTROLLERS.gptk` passou a ser consumido pelo adapter real:
  arquivo do dono → parser `nxinput 0.5.0` → ação semântica →
  KeyEvent/MotionEvent Android.
- Troca A/B e analógico foram exercitados até os sinks do jogo; controles
  pertencentes ao GPTK não percorrem a rota crua, evitando duplo input.
- Adapter promovido de `unimplemented_nonrelease` para
  `implemented_release` pelo mecanismo opt-in do `nxgenerator 0.2.15`.
- Build e gates materializam a árvore imutável do `nxinput 0.5.0` a partir de
  `FRAMEWORK-PIN.json`; o checkout móvel não entra mais no binário.
- A prova de imagem caseira foi substituída pelo adapter canônico de frame
  proof do `nxgl 0.2.16`, com amostra antes do present e recibo `VIDEO:`.
- O empacotador escolhe automaticamente `build_universal.sh`. O loader público
  permanece AArch64 com requisito máximo `GLIBC_2.27`.
- `adopt_single_channel` foi corrigido para `false`: a ponte de textura deste
  jogo é específica do port e não alega prova do adapter canônico do nxgl.
- Documentação pública reconciliada com o estado universal e sem dados locais
  de teste.
- Atualizado para `nxbootstrap 0.6.34` e NXExtract 1.2.21. O novo modo
  `reuse-only` autentica e migra dados já instalados sem abrir o extrator nem
  refazer a extração; qualquer dúvida sobre o conteúdo falha fechada.
- No dArkOSRE, o adapter agora vincula o par coerente
  `libEGL.so`/`libGLESv2.so` quando o SDL/KMSDRM roda sem providers herdados.
  Isso reproduz no launcher a recuperação fisicamente aprovada no Mali-G31 e
  deixa literalmente inalterado o caminho NextOS raw EGL do Mali-450.

### Regra de segurança preservada

A alteração em runtime da tabela Unity de plataforma de shader 5→9 continua
proibida: ela causou falha determinística no `libunity`. O caminho suportado é
o retag transacional e validado pelo NXExtract.
