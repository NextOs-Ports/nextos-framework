/*
 * pad_positional_fix.h — mapping SDL POSICIONAL derivado do evdev.
 *
 * PROBLEMA: em handhelds (ArkOS/ROCKNIX/muOS) o gamecontrollerdb e o
 * es_input.cfg sao autorados com ROTULO do aparelho (estilo Nintendo: "a" = o
 * botao da DIREITA, "b" = o de BAIXO, "x" = o de CIMA, "y" = o da ESQUERDA).
 * O SDL, e todo port que fala em A/B/X/Y, usa POSICAO (a=baixo, b=direita,
 * x=esquerda, y=cima). Carregar o db do sistema portanto entrega A/B **e** X/Y
 * trocados. No R36S isso fazia o "quadrado" do jogo cair no botao de cima.
 *
 * SOLUCAO: o driver de kernel nomeia os botoes por POSICAO REAL
 * (BTN_SOUTH/EAST/NORTH/WEST), o que e' fonte de verdade independente de
 * rotulo. Aqui lemos o evdev, reproduzimos a ordem de enumeracao do SDL para
 * achar o indice de cada botao, e reescrevemos SO' as entradas a/b/x/y do
 * mapping vigente — preservando o resto (back/start/thumbs/dpad/eixos), que o
 * db do sistema ja' acerta.
 *
 * Complementa [[pad_ordinal_fix.h]]: aquele trata kernel ANTIGO (3.14 sem
 * driver HID, botoes ordinais BTN_GAMEPAD+n, assinatura = BTN_C/BTN_Z
 * presentes); este trata kernel MODERNO com codigos semanticos. Os dois nunca
 * disparam no mesmo aparelho.
 *
 * USO (depois do SDL_Init, ANTES de SDL_IsGameController(index)):
 *   pad_positional_fix_apply(index, "COI");
 * Env: <PREFIX>_POSITIONAL_FIX=0 desliga.
 */
#ifndef PAD_POSITIONAL_FIX_H
#define PAD_POSITIONAL_FIX_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <SDL2/SDL.h>

#define PAD_POS_NBITS(x) (((x) + 8 * sizeof(long) - 1) / (8 * sizeof(long)))

static int pad_pos_test_bit(const unsigned long *b, int i) {
  return !!((b[i / (8 * sizeof(long))] >> (i % (8 * sizeof(long)))) & 1);
}

/* indice SDL de um codigo de botao: o SDL enumera [BTN_JOYSTICK..KEY_MAX) e
 * depois [0..BTN_JOYSTICK), incrementando um contador por bit presente. */
static int pad_pos_btn_index(const unsigned long *keyb, int code) {
  if (!pad_pos_test_bit(keyb, code))
    return -1;
  int n = 0;
  for (int i = BTN_JOYSTICK; i < KEY_MAX; i++) {
    if (i == code) return n;
    if (pad_pos_test_bit(keyb, i)) n++;
  }
  for (int i = 0; i < BTN_JOYSTICK; i++) {
    if (i == code) return n;
    if (pad_pos_test_bit(keyb, i)) n++;
  }
  return -1;
}

/* substitui "key:..." por "key:bN" no mapping, ou acrescenta se faltar */
static void pad_pos_set(char *map, size_t cap, const char *key, int idx) {
  if (idx < 0) return;
  char needle[24], repl[32];
  snprintf(needle, sizeof(needle), ",%s:", key);
  snprintf(repl, sizeof(repl), ",%s:b%d", key, idx);
  char *p = strstr(map, needle);
  if (p) {
    char *end = strchr(p + 1, ',');            /* fim do campo atual */
    char tail[512];
    snprintf(tail, sizeof(tail), "%s", end ? end : "");
    size_t head = (size_t)(p - map);
    snprintf(map + head, cap - head, "%s%s", repl, tail);
  } else {
    size_t len = strlen(map);
    snprintf(map + len, cap - len, "%s", repl);
  }
}

static void pad_positional_fix_apply(int index, const char *env_prefix) {
  char envname[64];
  snprintf(envname, sizeof(envname), "%s_POSITIONAL_FIX", env_prefix);
  const char *env = getenv(envname);
  if (env && (!strcmp(env, "0") || !strcasecmp(env, "off"))) return;

  SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(index);
  int vid = guid.data[4] | (guid.data[5] << 8);
  int pid = guid.data[8] | (guid.data[9] << 8);

  unsigned long keyb[PAD_POS_NBITS(KEY_MAX + 1)];
  int found = 0;
  for (int i = 0; i < 32 && !found; i++) {
    char path[64];
    snprintf(path, sizeof(path), "/dev/input/event%d", i);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) continue;
    struct input_id id;
    memset(&id, 0, sizeof(id));
    memset(keyb, 0, sizeof(keyb));
    if (ioctl(fd, EVIOCGID, &id) == 0 && id.vendor == vid && id.product == pid &&
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyb)), keyb) >= 0 &&
        pad_pos_test_bit(keyb, BTN_SOUTH) && pad_pos_test_bit(keyb, BTN_EAST) &&
        /* ⚠️ BTN_C/BTN_Z = assinatura de kernel ANTIGO com hid-generic, que
         * mapeia BTN_GAMEPAD+n pela ORDEM do report — ali os codigos NAO sao
         * semanticos (BTN_SOUTH e' so' "o primeiro botao do report", nao "o de
         * baixo") e quem corrige e' o pad_ordinal_fix. Se deixarmos os dois
         * atuarem no mesmo pad, um desfaz o outro. */
        !pad_pos_test_bit(keyb, BTN_C) && !pad_pos_test_bit(keyb, BTN_Z))
      found = 1;
    close(fd);
  }
  if (!found) return;   /* sem codigos semanticos -> nao ha' o que corrigir */

  /* posicao real, direto do kernel */
  int i_a = pad_pos_btn_index(keyb, BTN_SOUTH);  /* baixo    */
  int i_b = pad_pos_btn_index(keyb, BTN_EAST);   /* direita  */
  int i_x = pad_pos_btn_index(keyb, BTN_WEST);   /* esquerda */
  int i_y = pad_pos_btn_index(keyb, BTN_NORTH);  /* cima     */
  if (i_a < 0 || i_b < 0) return;

  char map[1024];
  char *cur = SDL_GameControllerMappingForDeviceIndex(index);
  if (cur) {
    snprintf(map, sizeof(map), "%s", cur);
    SDL_free(cur);
  } else {
    char gs[64];
    SDL_JoystickGetGUIDString(guid, gs, sizeof(gs));
    const char *nm = SDL_JoystickNameForIndex(index);
    char name[64];
    snprintf(name, sizeof(name), "%s", nm && *nm ? nm : "pad");
    for (char *c = name; *c; c++)
      if (*c == ',' || *c == ':') *c = ' ';
    snprintf(map, sizeof(map), "%s,%s,platform:Linux,", gs, name);
  }
  size_t l = strlen(map);
  if (l && map[l - 1] != ',') { map[l] = ','; map[l + 1] = 0; }

  pad_pos_set(map, sizeof(map), "a", i_a);
  pad_pos_set(map, sizeof(map), "b", i_b);
  pad_pos_set(map, sizeof(map), "x", i_x);
  pad_pos_set(map, sizeof(map), "y", i_y);

  int r = SDL_GameControllerAddMapping(map);
  fprintf(stderr, "[pad] positional fix (a=b%d b=b%d x=b%d y=b%d) r=%d: %s\n",
          i_a, i_b, i_x, i_y, r, map);
}

/* ---------------------------------------------------------------------------
 * Variante que corrige o ENV, e nao a tabela do SDL.
 *
 * POR QUE: no SDL 2.30 uma entrada vinda de `SDL_GAMECONTROLLERCONFIG` VENCE
 * `SDL_GameControllerAddMapping`. O PortMaster exporta esse env com o mapping
 * do CFW — autorado em ROTULO — entao a correcao acima e' registrada, o log diz
 * que aplicou (r=0), e o jogo continua recebendo o mapping trocado. Medido no
 * R36T: com o env, o botao de BAIXO chegava como "B"; sem o env, como "A".
 *
 * Como o env e' lido durante o SDL_Init, isto roda ANTES dele e nao pode usar
 * nada do SDL: a busca do pad e' por evdev puro, e o casamento com a entrada
 * certa do env e' pelo NOME do dispositivo (EVIOCGNAME), que e' o campo 2 de
 * cada linha de mapping. Entradas de OUTROS pads ficam intactas.
 * Desliga com <PREFIX>_POSITIONAL_FIX=0.
 * ------------------------------------------------------------------------- */
static void pad_positional_fix_env(const char *env_prefix) {
  char envname[64];
  snprintf(envname, sizeof(envname), "%s_POSITIONAL_FIX", env_prefix);
  const char *off = getenv(envname);
  if (off && (!strcmp(off, "0") || !strcasecmp(off, "off"))) return;

  const char *cfg = getenv("SDL_GAMECONTROLLERCONFIG");
  if (!cfg || !*cfg) return;   /* sem env, o caminho AddMapping ja' resolve */

  /* pad por evdev: precisa de codigos SEMANTICOS (SOUTH/EAST) e NAO pode ter
   * BTN_C/BTN_Z, assinatura do kernel antigo onde quem corrige e' o ordinal. */
  unsigned long keyb[PAD_POS_NBITS(KEY_MAX + 1)];
  char devname[128];
  int found = 0;
  for (int i = 0; i < 32 && !found; i++) {
    char path[64];
    snprintf(path, sizeof(path), "/dev/input/event%d", i);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) continue;
    memset(keyb, 0, sizeof(keyb));
    memset(devname, 0, sizeof(devname));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyb)), keyb) >= 0 &&
        ioctl(fd, EVIOCGNAME(sizeof(devname) - 1), devname) >= 0 &&
        pad_pos_test_bit(keyb, BTN_SOUTH) && pad_pos_test_bit(keyb, BTN_EAST) &&
        !pad_pos_test_bit(keyb, BTN_C) && !pad_pos_test_bit(keyb, BTN_Z))
      found = 1;
    close(fd);
  }
  if (!found) return;

  int i_a = pad_pos_btn_index(keyb, BTN_SOUTH);  /* baixo    */
  int i_b = pad_pos_btn_index(keyb, BTN_EAST);   /* direita  */
  int i_x = pad_pos_btn_index(keyb, BTN_WEST);   /* esquerda */
  int i_y = pad_pos_btn_index(keyb, BTN_NORTH);  /* cima     */
  if (i_a < 0 || i_b < 0) return;

  /* reescreve apenas as linhas cujo NOME casa com o pad que medimos */
  static char out[4096];
  out[0] = 0;
  size_t used = 0;
  const char *p = cfg;
  int touched = 0;
  while (*p) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    char line[1024];
    if (len >= sizeof(line)) len = sizeof(line) - 1;
    memcpy(line, p, len);
    line[len] = 0;

    /* campo 2 = nome */
    const char *c1 = strchr(line, ',');
    int match = 0;
    if (c1) {
      const char *c2 = strchr(c1 + 1, ',');
      size_t nlen = c2 ? (size_t)(c2 - c1 - 1) : strlen(c1 + 1);
      if (nlen == strlen(devname) && !strncmp(c1 + 1, devname, nlen)) match = 1;
    }
    if (match) {
      size_t l = strlen(line);
      if (l && line[l - 1] != ',') { line[l] = ','; line[l + 1] = 0; }
      pad_pos_set(line, sizeof(line), "a", i_a);
      pad_pos_set(line, sizeof(line), "b", i_b);
      pad_pos_set(line, sizeof(line), "x", i_x);
      pad_pos_set(line, sizeof(line), "y", i_y);
      touched = 1;
    }
    int n = snprintf(out + used, sizeof(out) - used, "%s%s", line, nl ? "\n" : "");
    if (n < 0 || (size_t)n >= sizeof(out) - used) return;  /* nao trunca o env */
    used += (size_t)n;
    if (!nl) break;
    p = nl + 1;
  }
  if (!touched) return;

  setenv("SDL_GAMECONTROLLERCONFIG", out, 1);
  fprintf(stderr, "[pad] env positional fix p/ '%s' (a=b%d b=b%d x=b%d y=b%d)\n",
          devname, i_a, i_b, i_x, i_y);
}

#endif /* PAD_POSITIONAL_FIX_H */
