# Ambiente AmberELEC/P4ELEC no PC

Lê da imagem **oficial** local o que essa família de firmwares entrega em
áudio e no ambiente de login. Nenhum dispositivo de áudio é aberto, nada é
montado e a imagem não entra no Git nem é redistribuída.

## Preparar e verificar

```sh
python3 framework/tests/device-environments/amberelec/prepare.py \
  --image /caminho/para/amberelec.img \
  --output /caminho/para/firmware-environments/amberelec-20230203

python3 framework/tests/device-environments/amberelec/verify.py \
  --environment /caminho/para/firmware-environments/amberelec-20230203
```

## O que fica provado

- **OpenAL**: a firmware entrega a biblioteca **e** um
  `/etc/openal/alsoft.conf` que força `drivers=alsa`. Esse arquivo vale para
  qualquer OpenAL que leia a configuração do sistema — inclusive um embutido
  no port, que passa a tocar no back-end da firmware em vez do seu;
- **ALSA/Pulse**: quais back-ends a imagem realmente traz, medidos, não
  supostos — é isso que dá sentido ao "quando aplicável";
- **ambiente de login**: o fragmento de perfil que altera `LD_LIBRARY_PATH`
  antes de qualquer port rodar. O ambiente herdado é real e tem dono;
- **a regra do framework**: o back-end é decidido pela capability
  `audio.embedded-openal`, **declarada pelo port e comprovada por probe** —
  nunca por nome de firmware ou de aparelho. O gate varre o gerador e o
  template atrás de qualquer seleção por `CFW_NAME`/`DEVICE_NAME` nas linhas
  de áudio e falha se aparecer uma.

O escudo de áudio do framework em si já tem gate próprio
(`nxbootstrap/tests/test-audio-shield.sh`) e não é duplicado aqui: este
ambiente prova os **fatos da firmware** que tornam aquele escudo necessário.

## Fronteira

Prova de host: arquivos, configuração declarada e ABI. Saída de som real,
mixer, latência e o comportamento em jogo continuam exigindo prova física
separada.
