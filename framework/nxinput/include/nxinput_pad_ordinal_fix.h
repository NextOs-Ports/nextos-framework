/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * nxinput_pad_ordinal_fix.h -- fonte unica da normalizacao "ordinal pad fix".
 *
 * Kernel antigo (3.14, Amlogic-old) sem driver HID especifico entrega o
 * controle externo pelo hid-generic e enumera os botoes pela ORDEM do report,
 * incluindo BTN_C/BTN_Z. Um mapping autorado em kernel moderno -- ou por
 * etiqueta fisica -- passa a apontar para outras posicoes e A/B, X/Y saem
 * trocados.
 *
 * A troca so acontece com a assinatura COMPLETA:
 *
 *   1. o pad exposto pela SDL tem VID/PID (GUID com CRC nao basta);
 *   2. existe um evdev com o mesmo VID/PID;
 *   3. esse evdev esta num BARRAMENTO EXTERNO (BUS_USB ou BUS_BLUETOOTH);
 *   4. o bitmap EV_KEY tem BTN_GAMEPAD e BTN_C ou BTN_Z.
 *
 * O item 3 e o gate BUS_HOST: controle EMBUTIDO (BUS_HOST, BUS_I2C, BUS_SPI,
 * BUS_VIRTUAL) nunca e tocado. Ha aparelhos cujo pad interno por gpio-keys
 * publica BTN_C/BTN_Z com layout semantico CORRETO: sem esse gate a correcao
 * trocaria os botoes de um controle que ja estava certo. Driver semantico
 * moderno tambem nao casa, porque nao publica BTN_C/BTN_Z. As tabelas evdev
 * reais que provam os dois lados ficam na fixture do framework, nunca aqui:
 * este modulo nao conhece nome de aparelho, firmware nem VID/PID.
 *
 * USO (depois dos includes de SDL2):
 *
 *   #include "nxinput_pad_ordinal_fix.h"
 *   ...
 *   nxinput_pad_ordinal_fix_apply(index, "TITANSOULS",
 *                                 NXINPUT_PAD_ORDINAL_LAYOUT_HID);
 *   if (!SDL_IsGameController(index)) ...  // ANTES de SDL_IsGameController
 *
 * A ordem de botoes do report tem duas classes conhecidas. Qual delas vale
 * para um pad e fato de IDENTIDADE (VID/PID), que pertence ao port ou ao
 * nxcompat -- nunca a este modulo. Por isso o layout e PARAMETRO: o padrao e
 * o HID e o chamador pede o alternativo quando a identidade que ele ja
 * possui manda. `<PREFIX>_PAD_ORDINAL_LAYOUT=hid|alt` sobrescreve em campo.
 *
 * Env: <PREFIX>_ORDINAL_FIX=0|off desliga; <PREFIX>_PAD_MAP (mapping manual)
 * tem prioridade e suprime a correcao. Retorno 1 = mapping trocado.
 *
 * O nucleo de decisao (bus, assinatura, mapping) nao chama SDL nem abre
 * device: e testado no PC com as tabelas evdev reais de cada aparelho em
 * framework/tests/fixtures/controls/pad-ordinal-v1.json. Defina
 * NXINPUT_PAD_ORDINAL_FIX_NO_SDL antes do include para compilar somente o
 * nucleo.
 */
#ifndef NXINPUT_PAD_ORDINAL_FIX_H
#define NXINPUT_PAD_ORDINAL_FIX_H

#define NXINPUT_PAD_ORDINAL_FIX_CONTRACT 1

#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef NXINPUT_PAD_ORDINAL_FIX_NO_SDL
#include <SDL2/SDL.h>
#endif

#if defined(__GNUC__)
#define NXINPUT_PAD_ORD_MAYBE_UNUSED __attribute__((unused))
#else
#define NXINPUT_PAD_ORD_MAYBE_UNUSED
#endif

#define NXINPUT_PAD_ORD_NBITS(x) \
  (((x) + 8 * sizeof(long) - 1) / (8 * sizeof(long)))

/* Numero de eventos varridos em /dev/input. */
#ifndef NXINPUT_PAD_ORD_MAX_EVENTS
#define NXINPUT_PAD_ORD_MAX_EVENTS 64
#endif

NXINPUT_PAD_ORD_MAYBE_UNUSED
static int nxinput_pad_ord_test_bit(const unsigned long *bits, int bit) {
  return !!((bits[bit / (8 * (int)sizeof(long))] >>
             (bit % (8 * (int)sizeof(long)))) &
            1UL);
}

/* Indice do eixo na SDL = posicao do codigo ABS presente no evdev; hats nao
 * viram eixo. */
NXINPUT_PAD_ORD_MAYBE_UNUSED
static int nxinput_pad_ord_abs_rank(const unsigned long *bits, int code) {
  int rank = 0;
  int i;
  if (!nxinput_pad_ord_test_bit(bits, code))
    return -1;
  for (i = 0; i < code; i++) {
    if (i >= ABS_HAT0X && i <= ABS_HAT3Y)
      continue;
    if (nxinput_pad_ord_test_bit(bits, i))
      rank++;
  }
  return rank;
}

/* GATE BUS_HOST: so barramento externo. Pad embutido fica intocado. */
NXINPUT_PAD_ORD_MAYBE_UNUSED
static int nxinput_pad_ordinal_bus_is_external(unsigned short bus) {
  return bus == BUS_USB || bus == BUS_BLUETOOTH;
}

/* Assinatura completa do HID ordinal antigo, ja com o gate de barramento. */
NXINPUT_PAD_ORD_MAYBE_UNUSED
static int nxinput_pad_ordinal_signature(unsigned short bus,
                                         const unsigned long *keybits) {
  if (!nxinput_pad_ordinal_bus_is_external(bus))
    return 0;
  if (!nxinput_pad_ord_test_bit(keybits, BTN_GAMEPAD))
    return 0;
  return nxinput_pad_ord_test_bit(keybits, BTN_C) ||
         nxinput_pad_ord_test_bit(keybits, BTN_Z);
}

/* --- Autoridade de A/B: o mapping completo e valido e SOBERANO ------------
 * Um mapping SDL/PortMaster completo (exportado em SDL_GAMECONTROLLERCONFIG)
 * ja resolve os ordinais; a normalizacao fisica NAO pode reinterpretar os
 * mesmos ordinais e trocar A/B uma SEGUNDA vez sobre ele. Estas tres funcoes
 * sao puras (sem SDL, sem abrir device) e decidem isso; o teste as exercita
 * com config sintetico. */

/* Um mapping liga `button` quando contem ",<button>:<src>" (ou no inicio) com
 * fonte real b/a/h seguida de digito. */
NXINPUT_PAD_ORD_MAYBE_UNUSED
static int nxinput_pad_ordinal_mapping_binds(const char *mapping,
                                             const char *button) {
  size_t blen;
  const char *p;
  if (!mapping || !button)
    return 0;
  blen = strlen(button);
  if (blen == 0)
    return 0;
  for (p = mapping; (p = strstr(p, button)) != NULL; p += 1) {
    const char *after = p + blen;
    if (*after != ':')
      continue;
    if (p != mapping && p[-1] != ',')
      continue;
    if ((after[1] == 'b' || after[1] == 'a' || after[1] == 'h') &&
        after[2] >= '0' && after[2] <= '9')
      return 1;
  }
  return 0;
}

/* Sovereignty is decided on A and B -- the two the second swap corrupts. */
NXINPUT_PAD_ORD_MAYBE_UNUSED
static int nxinput_pad_ordinal_mapping_is_complete(const char *mapping) {
  return nxinput_pad_ordinal_mapping_binds(mapping, "a") &&
         nxinput_pad_ordinal_mapping_binds(mapping, "b");
}

/* Does the PortMaster/CFW-exported SDL_GAMECONTROLLERCONFIG (`config`) carry a
 * COMPLETE mapping for this GUID? A built-in DB mapping is NOT authoritative
 * here -- only the exported config is. `config` is a parameter so the core is
 * testable without the environment. */
NXINPUT_PAD_ORD_MAYBE_UNUSED
static int nxinput_pad_ordinal_config_has_complete_mapping(
    const char *config, const char *guid_text) {
  const char *line;
  size_t glen;
  if (!config || !guid_text)
    return 0;
  glen = strlen(guid_text);
  if (glen == 0)
    return 0;
  for (line = config; line && *line;) {
    const char *end = strchr(line, '\n');
    size_t len = end ? (size_t)(end - line) : strlen(line);
    if (len > glen && strncmp(line, guid_text, glen) == 0 &&
        line[glen] == ',') {
      char buf[1024];
      if (len < sizeof(buf)) {
        memcpy(buf, line, len);
        buf[len] = '\0';
        if (nxinput_pad_ordinal_mapping_is_complete(buf))
          return 1;
      }
    }
    line = end ? end + 1 : NULL;
  }
  return 0;
}

/* The decision (pure): apply the physical normalization ONLY when the ordinal
 * signature matches AND no sovereign (authoritative complete) mapping is
 * present. force_opt_in overrides a sovereign mapping (the caller proved
 * incompatibility). */
NXINPUT_PAD_ORD_MAYBE_UNUSED
static int nxinput_pad_ordinal_should_apply(int signature,
                                            int sovereign_present,
                                            int force_opt_in) {
  if (!signature)
    return 0;
  if (sovereign_present && !force_opt_in)
    return 0;
  return 1;
}

/* Classes de ordem de report conhecidas. A escolha e do chamador. */
#define NXINPUT_PAD_ORDINAL_LAYOUT_HID 0
#define NXINPUT_PAD_ORDINAL_LAYOUT_ALT 1

/* Ordem FISICA da classe do pad (layout posicional Xbox, padrao dos ports). */
NXINPUT_PAD_ORD_MAYBE_UNUSED
static const char *nxinput_pad_ordinal_buttons(int layout) {
  if (layout == NXINPUT_PAD_ORDINAL_LAYOUT_ALT)
    return "x:b0,a:b1,b:b2,y:b3,leftshoulder:b4,rightshoulder:b5,"
           "lefttrigger:b6,righttrigger:b7,back:b8,start:b9,leftstick:b10,"
           "rightstick:b11,guide:b12,";
  return "a:b0,b:b1,x:b3,y:b4,leftshoulder:b6,rightshoulder:b7,"
         "lefttrigger:b8,righttrigger:b9,back:b10,start:b11,guide:b12,"
         "leftstick:b13,rightstick:b14,";
}

/* Override de campo, sem identidade: hid | alt. */
NXINPUT_PAD_ORD_MAYBE_UNUSED
static int nxinput_pad_ordinal_layout_from_env(const char *env_prefix,
                                               int layout) {
  char envname[64];
  const char *value;
  snprintf(envname, sizeof(envname), "%s_PAD_ORDINAL_LAYOUT",
           env_prefix ? env_prefix : "NXINPUT");
  value = getenv(envname);
  if (!value || !*value)
    return layout;
  if (!strcasecmp(value, "hid"))
    return NXINPUT_PAD_ORDINAL_LAYOUT_HID;
  if (!strcasecmp(value, "alt"))
    return NXINPUT_PAD_ORDINAL_LAYOUT_ALT;
  return layout;
}

/* Monta o mapping SDL. Devolve o tamanho escrito, ou -1 se nao coube. */
NXINPUT_PAD_ORD_MAYBE_UNUSED
static int nxinput_pad_ordinal_build_mapping(char *out, size_t size,
                                             const char *guid_text,
                                             const char *device_name,
                                             int layout,
                                             const unsigned long *absbits) {
  char name[64];
  char *cursor;
  int lx, ly, rx, ry;
  int off;

  if (!out || size == 0 || !guid_text || !absbits)
    return -1;
  snprintf(name, sizeof(name), "%s",
           device_name && *device_name ? device_name : "pad");
  for (cursor = name; *cursor; cursor++)
    if (*cursor == ',' || *cursor == ':')
      *cursor = ' ';

  off = snprintf(out, size, "%s,%s,platform:Linux,%s", guid_text, name,
                 nxinput_pad_ordinal_buttons(layout));
  if (off < 0 || (size_t)off >= size)
    return -1;

  lx = nxinput_pad_ord_abs_rank(absbits, ABS_X);
  ly = nxinput_pad_ord_abs_rank(absbits, ABS_Y);
  rx = nxinput_pad_ord_abs_rank(absbits, ABS_Z);
  ry = nxinput_pad_ord_abs_rank(absbits, ABS_RZ);
  if (rx < 0 || ry < 0) {
    rx = nxinput_pad_ord_abs_rank(absbits, ABS_RX);
    ry = nxinput_pad_ord_abs_rank(absbits, ABS_RY);
  }

#define NXINPUT_PAD_ORD_APPEND(fmt, value)                             \
  do {                                                                 \
    int written = snprintf(out + off, size - (size_t)off, fmt, value);  \
    if (written < 0 || (size_t)written >= size - (size_t)off)           \
      return -1;                                                        \
    off += written;                                                     \
  } while (0)

  if (lx >= 0)
    NXINPUT_PAD_ORD_APPEND("leftx:a%d,", lx);
  if (ly >= 0)
    NXINPUT_PAD_ORD_APPEND("lefty:a%d,", ly);
  if (rx >= 0)
    NXINPUT_PAD_ORD_APPEND("rightx:a%d,", rx);
  if (ry >= 0)
    NXINPUT_PAD_ORD_APPEND("righty:a%d,", ry);
  if (nxinput_pad_ord_test_bit(absbits, ABS_HAT0X) &&
      nxinput_pad_ord_test_bit(absbits, ABS_HAT0Y)) {
    int written = snprintf(out + off, size - (size_t)off,
                           "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,");
    if (written < 0 || (size_t)written >= size - (size_t)off)
      return -1;
    off += written;
  }
#undef NXINPUT_PAD_ORD_APPEND

  return off;
}

#ifndef NXINPUT_PAD_ORDINAL_FIX_NO_SDL
NXINPUT_PAD_ORD_MAYBE_UNUSED
static int nxinput_pad_ordinal_fix_apply(int index, const char *env_prefix,
                                         int layout) {
  char envname[64];
  const char *env;
  const char *usermap;
  SDL_JoystickGUID guid;
  int vid, pid;
  unsigned long keyb[NXINPUT_PAD_ORD_NBITS(KEY_MAX + 1)];
  unsigned long absb[NXINPUT_PAD_ORD_NBITS(ABS_MAX + 1)];
  int found = 0;
  unsigned short found_bus = 0;
  char guid_text[64];
  char mapping[512];
  int result;
  int i;
  int force_opt_in = 0;

  snprintf(envname, sizeof(envname), "%s_ORDINAL_FIX",
           env_prefix ? env_prefix : "NXINPUT");
  env = getenv(envname);
  if (env && (!strcmp(env, "0") || !strcasecmp(env, "off")))
    return 0;
  /* Explicit opt-in that overrides even a sovereign mapping: the caller has
   * proven the exported mapping is incompatible with the declared semantics. */
  force_opt_in = env && !strcasecmp(env, "force");
  snprintf(envname, sizeof(envname), "%s_PAD_MAP",
           env_prefix ? env_prefix : "NXINPUT");
  usermap = getenv(envname);
  if (usermap && *usermap)
    return 0;

  guid = SDL_JoystickGetDeviceGUID(index);
  vid = guid.data[4] | (guid.data[5] << 8);
  pid = guid.data[8] | (guid.data[9] << 8);
  if (!vid && !pid)
    return 0;

  for (i = 0; i < NXINPUT_PAD_ORD_MAX_EVENTS && !found; i++) {
    char path[64];
    int fd;
    struct input_id id;
    snprintf(path, sizeof(path), "/dev/input/event%d", i);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
      continue;
    memset(&id, 0, sizeof(id));
    memset(keyb, 0, sizeof(keyb));
    memset(absb, 0, sizeof(absb));
    if (ioctl(fd, EVIOCGID, &id) == 0 && id.vendor == vid &&
        id.product == pid &&
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyb)), keyb) >= 0 &&
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absb)), absb) >= 0 &&
        nxinput_pad_ordinal_signature(id.bustype, keyb)) {
      found = 1;
      found_bus = id.bustype;
    }
    close(fd);
  }
  if (!found)
    return 0;

  SDL_JoystickGetGUIDString(guid, guid_text, sizeof(guid_text));
  /* A/B authority (V3-CONTROLLERS-01): a complete PortMaster/CFW mapping
   * exported in SDL_GAMECONTROLLERCONFIG is SOVEREIGN. Never reinterpret the
   * same ordinals and swap A/B a second time on top of it. Only an explicit
   * force opt-in overrides. A built-in DB mapping is not authoritative here. */
  if (!nxinput_pad_ordinal_should_apply(
          1,
          nxinput_pad_ordinal_config_has_complete_mapping(
              getenv("SDL_GAMECONTROLLERCONFIG"), guid_text),
          force_opt_in)) {
    fprintf(stderr,
            "[pad] ordinal fix DEFERRED: sovereign SDL_GAMECONTROLLERCONFIG "
            "mapping present, no second A/B swap (vid=%04x pid=%04x guid=%s)\n",
            vid, pid, guid_text);
    return 0;
  }

  layout = nxinput_pad_ordinal_layout_from_env(env_prefix, layout);
  if (nxinput_pad_ordinal_build_mapping(mapping, sizeof(mapping), guid_text,
                                        SDL_JoystickNameForIndex(index),
                                        layout, absb) < 0) {
    fprintf(stderr, "[pad] ordinal fix aborted: mapping does not fit\n");
    return 0;
  }

  result = SDL_GameControllerAddMapping(mapping);
  fprintf(stderr,
          "[pad] ordinal fix (layout=%s external bus=%04x) vid=%04x pid=%04x "
          "result=%d: %s\n",
          layout == NXINPUT_PAD_ORDINAL_LAYOUT_ALT ? "alt" : "hid", found_bus,
          vid, pid, result, mapping);
  return result >= 0;
}
#endif /* NXINPUT_PAD_ORDINAL_FIX_NO_SDL */

#endif /* NXINPUT_PAD_ORDINAL_FIX_H */
