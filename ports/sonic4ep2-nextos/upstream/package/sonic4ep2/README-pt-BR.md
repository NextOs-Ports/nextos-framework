# Sonic The Hedgehog 4: Episode II - NextOS V6

Este e um pacote de compatibilidade PortMaster AArch64 para a versao Android
3.0.0-109 de Sonic The Hedgehog 4: Episode II. Ele contem o loader NextOS,
launcher, metadados, instalador e bibliotecas de audio redistribuiveis de
fallback. Ele **nao** inclui `libfox.so`, `data.obb`, APKs ou assets de runtime
da SEGA.

Versao obrigatoria: **Android 3.0.0-109, arm64-v8a**. O APK Android 2.0.0 e o
`main.22.com.sega.sonic4episode2.obb` antigo pertencem a engine armv7 e nao sao
compativeis com este pacote V6.

## Primeira instalacao

Instale o ZIP na pasta normal do PortMaster. O resultado deve ser:

```text
roms/ports/Sonic4EP2.sh
roms/ports/sonic4ep2/
```

Coloque uma fonte legal e completa da versao 3.0.0-109 ARM64 em
`roms/ports/sonic4ep2/`:

- `split_config.arm64_v8a.apk` e `split_packs.apk` da mesma instalacao;
- um export completo `.apks` ou `.apkm` contendo esses splits; ou
- um APK merged contendo `lib/arm64-v8a/libfox.so` e `assets/data.obb`.

Para descobrir os caminhos oficiais dos splits em um aparelho Android:

```text
adb shell pm path com.sega.sonic4episode2
adb pull "<caminho mostrado para split_config.arm64_v8a.apk>"
adb pull "<caminho mostrado para split_packs.apk>"
```

Os dois arquivos precisam vir da mesma instalacao. Mantenha um backup legal
fora da pasta do port, pois os arquivos de origem consumidos com sucesso podem
ser removidos ao final para liberar espaco.

Abra Sonic 4 EP2 pelo menu Ports. A V6 mostra a tela de instalacao antes das
verificacoes demoradas, separa validacao e extracao, confere a biblioteca e os
dados instalados e inicia o jogo. A interface visual e apenas feedback: se o
backend SDL/display nao puder abri-la, a instalacao continua em modo headless e
registra o diagnostico na pasta do jogo.

Reserve pelo menos 1,5 GB livres durante a primeira instalacao. A biblioteca e
os dados fornecidos pelo usuario ocupam aproximadamente 700 MB ao final.

## Arquitetura

O loader nativo `sonic4.arm64` mapeia a `libfox.so` Android fornecida pelo
usuario, implementa os servicos JNI/Android necessarios e apresenta o jogo por
SDL2 mais EGL/GLES. O port escolhe um contexto GLES real, adapta-se a Wayland,
KMSDRM ou framebuffer e usa a resolucao nativa do painel.

O audio passa pela ponte nativa do port. O pacote inclui fallbacks AArch64 de
mpg123, libogg, libvorbis e libvorbisfile para firmwares enxutos, mas continua
preferindo bibliotecas compativeis do sistema. O controle e a combinacao
Select + Start usam diretamente o caminho nativo do SDL.

A V6 preserva as correcoes ja estabelecidas de lifetime de texturas, limpeza
de cena/FBO, continuacao do titulo, input dos special stages, audio adaptativo e
selecao GLES no Mesa/Panfrost. Os videos Android nao sao reproduzidos pelo port.

## Controles

| Controle | Acao |
|---|---|
| Direcional / Analogico esquerdo | Mover e navegar |
| A | Pular / confirmar |
| B | Cancelar |
| Start | Pausar / confirmar no titulo |
| Select | Voltar |
| Select + Start | Sair para o frontend |

## Diagnostico

- Biblioteca ARM64 ausente: confirme que a fonte possui
  `split_config.arm64_v8a.apk`, nao somente o split armeabi-v7a.
- Dados ausentes: confirme que o mesmo export possui `split_packs.apk` com
  `assets/data.obb`.
- Erro na instalacao: consulte `roms/ports/sonic4ep2/bake.log`.
- Erro de jogo, video, audio ou controle: consulte
  `roms/ports/sonic4ep2/log.txt`.
- A segunda abertura nao deve pedir APKs quando
  `lib/arm64-v8a/libfox.so` e `data/data.obb` estiverem validos.
- O instalador nao exige GNU `stat`; em muOS ele usa Python para medir os
  arquivos sem reler o OBB inteiro a cada atualizacao de progresso.

## Build e pacote

O loader e compilado a partir de `ports/sonic4` com o toolchain de release
AArch64. O ZIP deve ser gerado somente pelo builder reproduzivel:

```bash
cd ports/sonic4
./package/build-package.sh
```

O builder usa uma allowlist explicita, valida ELF AArch64 e GLIBC no maximo
2.30, confere sintaxe Bash/Python e metadados, rejeita dados proprietarios,
gera manifesto de hashes e valida CRC, ordem, permissoes e topologia do ZIP.
Tambem produz um arquivo SHA-256 ao lado do pacote.

## Mapa do codigo

- `src/main.c`: lifecycle do loader, patches, input e frame loop.
- `src/setup_splash.c`: tela da primeira instalacao e protocolo de progresso.
- `src/egl_shim.c`: contexto SDL/EGL/GLES e apresentacao.
- `src/jni_shim.c`: superficie de compatibilidade Android/JNI.
- `src/sonic_audio.c`: musica, efeitos e saida de audio.
- `package/ports/Sonic4EP2.sh`: launcher PortMaster.
- `package/sonic4ep2/tools/sonic4ep2_extract.sh`: instalador BYO-data.
- `package/build-package.sh`: builder deterministico sem dados do jogo.

Consulte `README.md` para a versao em ingles e `licenses/` para os avisos de
codigo, dependencias, imagens de frontend e dados do jogo.
