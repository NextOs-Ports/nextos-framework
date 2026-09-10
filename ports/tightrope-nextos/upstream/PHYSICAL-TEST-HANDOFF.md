# Tightrope Theatre 1.0.5-test.1 — ROCKNIX physical test handoff

Use the exact candidate ZIP and its `.sha256` on each explicitly authorized
target. Do not publish based on a rebuilt or manually modified archive.

The exact 1.0.4 loader was physically validated on ArkOS/R36S through the
public launcher: menu isolation, native flow to Level1/Level2, exact analogue
thresholds, D-pad movement/jump, A jump, audio and status-0 SELECT+START exit.
Version 1.0.5-test.1 preserves that adapter and changes only host-controller
discovery/mapping. It remains unverified on ROCKNIX until this exact ZIP proves
right-stick cursor movement, R1/R3 click and SELECT+START exit.

## Non-destructive preparation

1. Confirm no old `tightrope-nextos`, launcher, extractor or game process is
   alive. Never start a second instance.
2. Back up `tightrope/home/` and the legal XAPK from `tightrope/gamedata/`.
3. Install the candidate cleanly, then restore only `home/` and the XAPK. Do
   not overlay retired `run.sh` or `ports_scripts` files.
4. Record the XAPK size/SHA-256 and one save-file size/SHA-256 before testing.

## First opening — complete extraction

1. Start from the frontend and capture the graphical SDL2 NXExtract screen.
   The capture must show the established graphical layout/colors, not terminal
   text or an ASCII fallback.
2. Let extraction reach its complete transactional commit. Do not interrupt it.
3. Confirm the source XAPK and existing saves have exactly their original
   size/SHA-256. Keep the XAPK in `gamedata/`.
4. Capture the separate colored bilingual NextOS/RETRO ELITE splash and time it
   at five seconds, then allow the native game to open normally.
5. Check: right stick moves the polished arrow continuously and R1/R3 click.
   On title/settings/selectors, D-pad, left stick and A must remain neutral.
   In a level, D-pad left/right move precisely, D-pad up and A jump, the less
   sensitive left stick moves progressively, audio is present, and
   SELECT+START saves and returns to the frontend.

## Second opening — validated fast path

1. Open the port again without changing or deleting extracted data.
2. Confirm there is no full extraction pass.
3. Capture and time the colored NextOS/RETRO ELITE splash at five seconds.
4. Recheck cursor movement and SELECT+START, then reload the saved progress.

Collect `log.txt`, `log.prev.txt`, `nxextract.log`, the two UI captures, timing,
device/CFW identity and the exact ZIP SHA-256. A result is accepted only when
the same archive passes on that device. No network address belongs in evidence
or release files.

## Roteiro físico em português

Use o ZIP candidato e o `.sha256` exatos em cada alvo explicitamente
autorizado. O loader 1.0.4 exato já foi validado fisicamente no ArkOS/R36S pelo
launcher público: menus isolados, fluxo nativo até Level1/Level2, limiares do
analógico, movimento/salto no D-pad, salto no A, áudio e saída SELECT+START com
status 0. A versão 1.0.5-test.1 preserva esse adapter e muda somente a descoberta
e o mapping do controle host. Ela continua pendente no ROCKNIX até o ZIP exato
provar cursor no analógico direito, clique R1/R3 e saída SELECT+START. Confirme
que não há outra
instância, faça backup de `home/` e do XAPK, instale limpo e devolva somente
saves e XAPK. Nunca apague nem altere o XAPK-fonte.

Na primeira abertura, fotografe a interface gráfica SDL2 original do NXExtract,
espere a extração completa terminar e confirme por tamanho/SHA-256 que XAPK e
saves ficaram intactos. Fotografe a tela colorida bilíngue NextOS/RETRO ELITE
separada e cronometre cinco segundos. No menu, teste seta no analógico direito
e clique R1/R3, confirmando que D-pad, analógico esquerdo e A ficam neutros.
Na fase, teste movimento preciso em esquerda/direita do D-pad, salto no D-pad
para cima e no A, analógico esquerdo menos sensível, áudio, save e saída com
SELECT+START.

Na segunda abertura, confirme o caminho rápido sem nova extração, fotografe e
cronometre novamente a tela colorida por cinco segundos, recarregue o save e
repita cursor e saída. Entregue logs, capturas, tempos, identidade do firmware e
SHA-256 do ZIP exato; não registre IP ou hostname.
