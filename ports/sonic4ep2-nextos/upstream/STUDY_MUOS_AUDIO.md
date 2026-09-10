# muOS audio path — estudo + fix (RESOLVIDO, provado)

Data: 2026-07-01. Device do tester = família **Allwinner H700** (RG35XX-H / RG40XX-H;
os cards `audiocodec` + `ahubhdmi` do log são nomenclatura Allwinner `ahub`).

## TL;DR

O "MuOS no audio" do nosso port 32-bit **não é código de áudio nosso** — é multiarch:
o muOS roda **PipeWire**, o **SDL2 32-bit** dele só tem backend **ALSA**, e o ALSA
`default` roteia p/ o PipeWire por plugin. Um processo **32-bit** precisa carregar o
plugin ALSA + módulos PipeWire + plugins SPA das pastas **`/usr/lib32/...`**; sem isso
pega as **64-bit** → `wrong ELF class` / `can't make support.system handle` →
`snd_pcm_open("default") = -2` = **MUDO** (ou cai no HDMI).

**Fix (só launcher, sem rebuild):** marcar `PORT_32BIT="Y"` no `.sh` (o muOS lê isso e
seta `PIPEWIRE_MODULE_DIR`/`SPA_PLUGIN_DIR` p/ lib32) **+** setar defensivamente
`PIPEWIRE_MODULE_DIR` / `SPA_PLUGIN_DIR` / `ALSA_PLUGIN_DIR` p/ os dirs lib32.
Entregue no **v4.5**.

## O caminho de áudio do muOS (fonte: MustardOS/internal + imagem RG35XX-H)

- Boot `script/init/S80pipewire.sh`: sobe **pipewire + wireplumber**. **NÃO** sobe
  `pipewire-pulse` (o `pipewire.conf` não carrega `module-protocol-pulse`) → **não há
  socket pulse**. `XDG_RUNTIME_DIR=/run`, socket `/run/pipewire-0`.
- `/etc/asound.conf`: `pcm.!default { type plug; slave.pcm { type pipewire } }` →
  todo ALSA `default` cai no PipeWire.
- O sink (speaker `pf_internal` vs HDMI `pf_external`) é escolhido pelo **wireplumber**
  (`wpctl set-default`), não pelo app.
- Ports 32-bit: `script/launch/ext-general.sh` faz
  `grep -q '^[[:space:]]*[^#]*PORT_32BIT="Y"' "$FILE"` e, se casar, exporta
  `PIPEWIRE_MODULE_DIR=/usr/lib32/pipewire-0.3` e `SPA_PLUGIN_DIR=/usr/lib32/spa-0.2`.
  **NÃO seta `ALSA_PLUGIN_DIR`.**

## Backends do SDL2 do muOS (imagem, `strings` nos .so)

| SDL2 | backends compilados |
|------|---------------------|
| 64-bit (`/usr/lib`)  | (não checado a fundo; irrelevante p/ nós) |
| **32-bit (`/usr/lib32`)** — o que NOSSO port usa | **só `alsa`** |

Ou seja no muOS: `AUDIO_DRIVER=pulse` e `AUDIO_DRIVER=pipewire` **FALHAM** (não existe
esse backend no SDL 32-bit). Só **`alsa`** funciona — e o auto do SDL já escolhe alsa
(é o único). Não precisa setar `AUDIO_DRIVER` se o fix multiarch estiver aplicado.

## Prova experimental (qemu-arm sobre a imagem muOS montada)

Teste 32-bit ARM (`snd_pcm_open("default")`) rodado com `qemu-arm -L <rootfs>`:

| Cenário | env extra | `snd_pcm_open("default")` |
|---------|-----------|---------------------------|
| **A** (nosso port ANTES) | nada | **-2 (ENOENT) = MUDO** |
| **B** | `ALSA_PLUGIN_DIR`+`PIPEWIRE_MODULE_DIR`+`SPA_PLUGIN_DIR`=lib32 | **0 (Success)** |
| **C** (só o que o muOS `PORT_32BIT` seta) | `PIPEWIRE_MODULE_DIR`+`SPA_PLUGIN_DIR`=lib32 | **0 (Success)** |

Fatos de ELF que explicam: `/usr/lib/alsa-lib/libasound_module_pcm_pipewire.so` é
**aarch64** → `dlopen` num processo 32-bit dá `wrong ELF class: ELFCLASS64`; o
`libasound` 32-bit tem como plugin-dir default **`/usr/lib/alsa-lib`** (64-bit). O
`libspa-support.so` (o `support.system` que faltava no cenário A) só existe 32-bit em
`/usr/lib32/spa-0.2/support/`.

## Fix aplicado (launcher, guardado por existência — inofensivo nos outros devices)

```sh
PORT_32BIT="Y"
for _pwl in /usr/lib32 /usr/lib/arm-linux-gnueabihf /usr/local/lib/arm-linux-gnueabihf; do
  [ -d "$_pwl/pipewire-0.3" ] && export PIPEWIRE_MODULE_DIR="$_pwl/pipewire-0.3"
  [ -d "$_pwl/spa-0.2" ] && export SPA_PLUGIN_DIR="$_pwl/spa-0.2"
  [ -e "$_pwl/alsa-lib/libasound_module_pcm_pipewire.so" ] && export ALSA_PLUGIN_DIR="$_pwl/alsa-lib"
done
```

- **Mali-450 .79** (EmuELEC/pulse): sem `pipewire-0.3` 32-bit → guarda pula → usa pulse. OK.
- **R36S ArkOS** (só alsa, sem pipewire): guarda pula → alsa `default` (hw). OK.
- **ArchR** (pipewire): seta dirs 32-bit corretos. OK.
- **muOS**: ativa lib32 → `default` abre → wireplumber roteia p/ o speaker. **RESOLVE.**
