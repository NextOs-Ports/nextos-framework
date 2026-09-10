# HANDOFF — Save-fix + Controles (2026-06-18 noite)

## TL;DR pro porter
- **Controles CONSERTADOS.** Eu tinha quebrado criando gamepads virtuais que clonavam o id do
  teu pad (0810:0001) → SDL confundia. O **reboot limpou**. Confirmei: pad reconhecido como
  `id=0 isGameController=1`, input limpo (só o pad real).
- **Save-fix DEPLOYADO no device** (libWwise nova c/ roteamento de sigaction + SOR4.dll
  save-enabled). **Boot OK no título, sem crash.** Falta só VOCÊ jogar a fase pra ver se o crash
  do save sumiu (eu não consegui navegar sozinho sem quebrar o controle — lição aprendida).
- **Rede de segurança:** se o controle falhar, roda no device:
  `bash /storage/roms/restaurar_controles.sh` → volta os binários que funcionavam o dia todo
  (libWwise antiga + DLL estável, SEM save-fix).

## Como TESTAR o save (você, jogando)
1. Abre o SoR4 pela ES (controle funciona agora).
2. Título → modo (Story) → **Normal** → personagem → fase. (Era no "clicar Normal" que crashava =
   criação do save.)
   - **Se NÃO crashar e entrar na fase = o fix do sigaction funcionou.** 🎉
   - Se crashar = me avisa, o crash-handler loga em `log.txt` (lança com `WWISE_CRASHLOG=1`).
3. Persistência: completa/joga a fase 1, sai (SELECT+START), reabre. Se o progresso/desbloqueio
   ficou = save PERSISTE (o `Save.bin` grava em `$HOME=$GAMEDIR/save`, vfat, ok pq o pulse usa
   PULSE_SERVER e não cria symlink).

## O que está deployado no device (.127)
- `host_pkg/libs/libWwise.so` = **4f5e6e96** (NOVA: roteia `sigaction`→`my_sigaction` + gate
  CUP_NOSIGH/CUP_GCSIG → o handler SIGSEGV do .NET CoreCLR sobrevive ao Wwise). Build glibc≤2.27
  (Docker Buster), universal. Fonte: working tree de `~/nextos_ports_android/ports/sor4wwise/`
  (`imports.gen.c` + `wwise_native.c` MODIFICADOS, **não commitados**).
- `host_pkg/SOR4.dll` = **1484800** (save-enabled: extract.src faz noopm AndroidServices.* +
  skipcall save_save_game do startup, mantendo o save LOCAL).
- Backups no device: `/storage/roms/libWwise.so.OLD-good` (1ca53e6c) + `/storage/roms/SOR4.dll.keep`
  (1483776 estável).

## Causa-raiz do crash do save (hipótese do fix)
`libWwise.real.so` chamava `sigaction` direto na glibc (não roteado) → (1) ABI mismatch
bionic(32B)↔glibc(152B) no oldact corrompia a stack; (2) o handler SIGSEGV do Wwise sobrescrevia
o do .NET → null-check/GC do .NET virava crash. O fix roteia `sigaction` pela shim + CUP_NOSIGH
bloqueia o Wwise de instalar handler de SIGSEGV/ABRT/BUS. **Não testado em gameplay ainda.**

## Auto-input (por que não testei sozinho) — ver doc da série
Ver doc interna `SERIE-1-auto-input-em-jogos.md`. Resumo: criar pad
virtual clonando 0810:0001 QUEBRA o controle. O certo é `inject.py` direto no `/dev/input/event2`
(id=0) ou `evcap.py` capturando a navegação + clonar. O título do SoR4 não avançou só com
A/START injetado → falta capturar a ação certa com uma demonstração 1x.

## Pendências
- [ ] Você jogar a fase e confirmar: crash do save sumiu? save persiste?
- [ ] Se OK: commitar `imports.gen.c`+`wwise_native.c` (SEM co-autor, master) + atualizar o zip
      de distribuição com a libWwise nova.
- [ ] Se o controle der problema: `bash /storage/roms/restaurar_controles.sh` + me avisar.
- [ ] (Opcional) auto-input via captura+clone (evcap) pra eu testar sozinho no futuro.
