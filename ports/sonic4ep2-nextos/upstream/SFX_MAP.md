# Sonic 4 EP2 - SFX map

## Fonte
Fonte principal: tabela Java oficial dentro do APK.

- APK: `sonic-the-hedgehog-4-episode-ii-2.0.0.apk`
- DEX extraido: `/tmp/sonic4-classes2.dex`
- Classe: `com/mineloader/fox/AudioDataTbl`
- Dump local: `/tmp/sonic4-dex-sfx-map.tsv`
- Metodos importantes:
  - `GetCueMap_S4EP1_SND_SE`
  - `GetCueMap_S4EP2Default`
  - `GetCueMap_S4EP2_SND_SE`
  - `GetCueMap_S4EP2_SND_SE_Z1`
  - `GetCueMap_S4EP2_SND_SE_Z2`
  - `GetCueMap_S4EP2_SND_SE_Z3`
  - `GetCueMap_S4EP2_SND_SE_Z4`
  - `GetCueMap_S4EP2_SND_SE_ZF`
  - `GetCueMap_S4EP2_SND_SNG`

Resultado do dump: 772 linhas, 288 cues unicos e 10 cues com conflito por banco/zona.

## Mapa aplicado
Este e o mapa base aplicado em `src/sonic_audio.c`. Ele cobre os cues comuns e Z1 que ja foram
necessarios nos testes atuais.

| Cue | Arquivo |
| --- | --- |
| `Ok` | `S4EP2FX_001_SHSY08_22.OGG` |
| `Cancel` | `S4EP2FX_002_SHSY09_22.OGG` |
| `Pause` | `S4EP2FX_004_SHSY10_22.OGG` |
| `Special_1up` | `S4EP2FX_008_S1BF_44.OGG` |
| `Jump` | `S4EP2FX_009_SK62_44.OGG` |
| `Ring1` | `S4EP2FX_011_SK33_44.OGG` |
| `Ring2` | `S4EP2FX_012_SKB9_44.OGG` |
| `Damage1` | `S4EP2FX_013_S1A3_44.OGG` |
| `Damage3` | `S4EP2FX_014_S1B2_44.OGG` |
| `Homing` | `S4EP2FX_015_SHPLSP01_22.OGG` |
| `Transform` | `S4EP2FX_016a_S2_645F_44.OGG` |
| `Enemy` | `S4EP2FX_017a_S2_3441_44.OGG` |
| `Barrier` | `S4EP2FX_018_SK3A_44.OGG` |
| `Spin` | `S4EP2FX_019_SKAB_44.OGG` |
| `Dash1` | `S4EP2FX_020_SKB6_44.OGG` |
| `Dash2` | `S4EP2FX_021_SK3C_44.OGG` |
| `Damage2` | `S4EP2FX_022_S1A6_44.OGG` |
| `Attention` | `S4EP2FX_024_S1C2_44.OGG` |
| `Breathe` | `S4EP2FX_025_S1AD_44.OGG` |
| `Spring` | `S4EP2FX_067_SKB1_44.OGG` |
| `DashPanel` | `S4EP2FX_069_SHCN000B_22.OGG` |
| `BreakGround` | `S4EP2FX_070_S1B9_44.OGG` |
| `Catapult` | `S4EP2FX_084a_S2_6762_44_CaSINO_CaTaPULT.OGG` |
| `Ring1L` | `S4EP2FX_112a_S2_2235_44L.OGG` |
| `Ring1R` | `S4EP2FX_113a_S2_2235_44R.OGG` |
| `Catapult1` | `S4EP2FX_120a_SKA4_EDIT_44LP.OGG` |
| `LockedOn` | `S4EP2FX_144a_COLORS_OBJ_LOCKON_22.OGG` |
| `TlsScrew` | `S4EP2FX_151_V2_1208.OGG` |
| `TlsProp` | `S4EP2FX_151X_S3_D1.OGG` |
| `Waterdash01` | `S4EP2FX_158X_S2_81_70_1210V3.OGG` |
| `Coop01` | `S4EP2FX_202_332_V1_1028.OGG` |
| `Coop02` | `S4EP2FX_203_V2_1207.OGG` |
| `Coop04` | `S4EP2FX_205_V1_1201_COOP_SPIN_ST.OGG` |
| `Coop05` | `S4EP2FX_206_V6_1225.OGG` |
| `Double01` | `S4EP2FX_207_V2_1217.OGG` |
| `Double02` | `S4EP2FX_208_V2_1207.OGG` |
| `Double03` | `S4EP2FX_209_V2_1217.OGG` |
| `MS_Jump` | `S4EP2FX_210_METaLSONIC_JUMP_44.OGG` |
| `MS_Ring2` | `S4EP2FX_212_V1_1028.OGG` |
| `MS_Burner1` | `S4EP2FX_219_V2_HEaD.OGG` |
| `MS_Burner2` | `S4EP2FX_220_V1_1028.OGG` |
| `LightRing01` | `S4EP2FX_246_V1_1117.OGG` |
| `LightRing02` | `S4EP2FX_247_V4_1221.OGG` |
| `LightRing03` | `S4EP2FX_248_V1_1028.OGG` |
| `ItemBox_Dbl` | `S4EP2FX_266_V2 1210.OGG` |
| `RedStar01` | `S4EP2FX_378_V1_1028.OGG` |
| `Double04` | `S4EP2FX_381_V1_1210.OGG` |
| `Double05` | `S4EP2FX_382_1_V1_1210.OGG` |
| `Double06` | `S4EP2FX_383_V1_1210.OGG` |
| `RingGate` | `S4EP2FX_403_V1_1229.OGG` |

## Validado no device
Teste bom:

```sh
SONIC_EXTRA='SONIC_NOFAKESOUND=1 SONIC_AUDIOLOG=1 SONIC_AUTORIGHT_AFTER=1150 SONIC_AUTOJUMP_AT=1240 SONIC_INPUTLOG=1' sh ./runsonic.sh 150
```

Artefatos:

- `/tmp/sonic4-input-a-game1.log`
- `/tmp/sonic4-input-a-game1.png`

Observado no log:

- `Ok` decodifica `S4EP2FX_001_SHSY08_22.OGG`
- `Jump` decodifica `S4EP2FX_009_SK62_44.OGG`
- `Ring1L` decodifica `S4EP2FX_112a_S2_2235_44L.OGG`
- `Ring1R` decodifica `S4EP2FX_113a_S2_2235_44R.OGG`
- `LockedOn` decodifica `S4EP2FX_144a_COLORS_OBJ_LOCKON_22.OGG`
- `Spring` decodifica `S4EP2FX_067_SKB1_44.OGG`
- nenhum `unmapped sfx` nesse trecho.

## Conflitos por banco/zona
O dump do DEX mostrou 10 cues que podem mudar conforme `asyncBuildSpData` carrega o banco da zona:

- `TlsScrew`
- `TlsProp`
- `B_Piller01`
- `Jetwall05`
- `e2_Boss1_17`
- `MetalUnit02`
- `SandTrank02`
- `e2_Boss3_14`
- `e2_Boss4_06`
- `Uri03`

Proximo passo tecnico quando algum deles aparecer: guardar o banco atual vindo de
`AudioHelper.asyncBuildSpData(path)` e resolver `cue -> arquivo` com chave `(banco, cue)`.

## Flags uteis
- `SONIC_AUDIOLOG=1`: loga AudioHelper, decodes, unmapped e overrides.
- `SONIC_SFX_OVERRIDE='Cue=Arquivo.OGG'`: testa uma troca sem recompilar.
- `SONIC_AUTORIGHT_AFTER=N`: segura direita apos a fase iniciar.
- `SONIC_AUTOJUMP_AT=N`: aperta A de gameplay em uma janela curta.
- `SONIC_AUTOPAUSE_AT=N`: diagnostico de Start/Pause.
- `SONIC_INPUTLOG=1`: loga mascara/input em gameplay.
- `SONIC_IOLOG=1`: loga `fopen`; nao cobre leituras internas do LPK por `tsReadFile`.
