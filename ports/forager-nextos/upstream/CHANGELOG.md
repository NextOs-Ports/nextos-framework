# Changelog

## 1.0.4 — 2026-08-18

### Corrigido — saída SELECT+START definitiva (chord canônico do framework)
- Adota o módulo **canônico** do framework (`nxinput_evdev_chord.h` v2, nxinput
  0.4.0), acabando com as regressões de saída entre CFWs:
  - **SDL é a autoridade** quando há pad aberto: o chord é
    `SDL_GameControllerGetButton(BACK) && (START)` por **estado**, mais o botão
    **cru** do joystick nos índices que o próprio mapping declara para back/start
    (cobre controle virtual do muOS). Dispara em ~50 ms, **sem hold longo**.
  - **evdev cru só como fallback** quando não há pad SDL.
  - **Nunca** vigia `BTN_SELECT/START` literais com pad aberto (na família H700
    esses códigos são L2/R2 → não há mais "L2+R2 sai").
- **Causa-raiz das regressões 1.0.1→1.0.3:** o header antigo derivava o código
  evdev do índice SDL usando a enumeração da SDL vanilla, mas os CFWs Batocera-like
  (Knulli/muOS) aplicam o patch `sdl2_input_as_retroarch_udev` que enumera
  `0..KEY_MAX` crescente. Mesmo device, mesmo mapping, tabela índice→código
  diferente: SELECT+START virava L3+L2 e "não saía". O caminho por estado do SDL
  não depende dessa tabela.
- **Diagnóstico de controle** completo no log (nome, GUID, mapping, binds, ids e
  lista de códigos evdev de cada pad, mais as duas tabelas índice→código), para
  achar bug de saída/controle sem o device.
- Saída limpa (status 0) reconfirmada no R36S (GO-Super) lançando pelo
  EmulationStation.

## 1.0.2 — 2026-08-18

### Corrigido / robustez de saída (relato: RG40XX-H / muOS não fechava)
- **Saída SELECT+START mais robusta em controles virtuais (muOS/gptokeyb).** Além
  do caminho por `GameController`, o chord agora lê também os **botões crus do
  joystick** nos mesmos índices que o mapping diz serem back/start — cobre CFWs em
  que a camada de GameController do controle virtual ("muOS-Keys") não propaga
  BACK/START. Tempo de hold reduzido para ~0,75 s.
- **Chord por evdev passa a vigiar DOIS combos em paralelo** — o derivado do bind
  SDL **e** o literal (`BTN_SELECT/BTN_START`/TRIGGER_HAPPY) — excluindo os códigos
  de L2/R2 do device (via bind de trigger) para nunca reintroduzir o "L2+R2 sai".
- **Handler de sinal (SIGTERM/SIGINT/SIGHUP) no binário:** se a frontend fechar o
  port matando o processo, ele encerra limpo com `_exit(0)` (o save já está no
  disco), mesmo que o laço principal esteja preso.

## 1.0.1 — 2026-08-17

### Corrigido
- **Apagar save agora funciona de verdade** — repetível e sem travar/fechar o jogo.
  O Forager confirma o apagar por `show_question_async`, que no Android chama o
  método nativo `RunnerJNILib.ShowQuestionAsync`; esse método faltava no loader,
  então a confirmação nunca respondia e o slot de save travava (apertar X "bipava"
  mas não apagava, e depois o A não entrava mais no jogo).
  - Implementados `ShowQuestion`, `ShowQuestionAsync`, `ShowMessage` e
    `ShowMessageAsync` no `RunnerJNILib`.
  - A resposta do diálogo é entregue pela via oficial do runtime
    (`InputQuery::SetResult`), **adiada para fora do `Kick()`** do laço principal
    para evitar reentrância/use-after-free, e com a string de resultado alocada
    no heap (o runtime faz `free()` nela) — isso corrige tanto o crash
    (`corrupted size vs prev_size`) quanto o travamento que aparecia a partir do
    **segundo** apagar na mesma sessão.
- **Exit chord SELECT+START** derivado diretamente do bind do SDL, corrigindo
  aparelhos em que o jogo saía com **L2+R2** em vez de SELECT+START.

### Mudado
- **Pasta do port renomeada de `forager` para `nxforager`**, para conviver lado a
  lado com instalações do PortMaster sem conflito de pasta. Migração para o formato
  de release "definitivo" (launcher/manifesto/estrutura de pacote).
- Guia de instalação (`INSTALLATION.md`) incluído no pacote de release.

### Notas
- Continua exigindo no máximo GLIBC 2.28; sem RPATH/RUNPATH e sem dados
  proprietários no pacote. O `port-env.sh` não cria swap.

## 1.0.0 — 2026-08-13

- First private universal PortMaster release.
- Preserves the Android GameMaker startup and runtime flow through a reproducible
  ARMv7 gmloader-next adapter.
- Adds transactional NXExtract 1.2.6 owner-package discovery for loose APK and
  base APK inside APKM/APKS/XAPK containers.
- Validates the exact 24 consumed owner members and emits one deterministic
  STORE-only runtime archive.
- Provides GLES2 video, music and sound effects, native SDL controllers, saves,
  clean Select+Start exit and twelve persistent language choices.
- Requires at most GLIBC 2.28; no RPATH/RUNPATH or proprietary data is shipped.
