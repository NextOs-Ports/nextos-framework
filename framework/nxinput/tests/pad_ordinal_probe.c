/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Probe determinista do nucleo do ordinal pad fix. Nao abre device, nao usa
 * SDL: recebe a tabela evdev de um caso real e imprime a decisao e o mapping.
 *
 *   pad_ordinal_probe --bus 0x0003 --layout hid|alt --keys 0x130,0x132
 *                     --abs 0x00,0x01 --guid <texto> --name <nome>
 */
#define NXINPUT_PAD_ORDINAL_FIX_NO_SDL 1
#include "nxinput_pad_ordinal_fix.h"

#include <errno.h>
#include <limits.h>

static unsigned long probe_keys[NXINPUT_PAD_ORD_NBITS(KEY_MAX + 1)];
static unsigned long probe_abs[NXINPUT_PAD_ORD_NBITS(ABS_MAX + 1)];

static int probe_set_bits(const char *list, unsigned long *bits, int maximum) {
  const char *cursor = list;
  while (cursor && *cursor) {
    char *end = NULL;
    long value = strtol(cursor, &end, 0);
    if (end == cursor)
      return -1;
    if (value < 0 || value > maximum)
      return -1;
    bits[value / (8 * (int)sizeof(long))] |=
        1UL << (value % (8 * (int)sizeof(long)));
    cursor = (*end == ',') ? end + 1 : end;
  }
  return 0;
}

int main(int argc, char **argv) {
  long bus = -1;
  long layout = 0;
  const char *guid = "00000000000000000000000000000000";
  const char *name = "pad";
  const char *config = NULL;
  long force = 0;
  char mapping[512];
  int i;
  int signature;
  int sovereign;

  for (i = 1; i < argc; i += 2) {
    const char *value = (i + 1 < argc) ? argv[i + 1] : NULL;
    if (!value) {
      fprintf(stderr, "missing value for %s\n", argv[i]);
      return 2;
    }
    if (!strcmp(argv[i], "--bus"))
      bus = strtol(value, NULL, 0);
    else if (!strcmp(argv[i], "--layout"))
      layout = !strcmp(value, "alt") ? NXINPUT_PAD_ORDINAL_LAYOUT_ALT
                                     : NXINPUT_PAD_ORDINAL_LAYOUT_HID;
    else if (!strcmp(argv[i], "--guid"))
      guid = value;
    else if (!strcmp(argv[i], "--name"))
      name = value;
    else if (!strcmp(argv[i], "--config"))
      config = value;
    else if (!strcmp(argv[i], "--force"))
      force = strtol(value, NULL, 0);
    else if (!strcmp(argv[i], "--keys")) {
      if (probe_set_bits(value, probe_keys, KEY_MAX))
        return 2;
    } else if (!strcmp(argv[i], "--abs")) {
      if (probe_set_bits(value, probe_abs, ABS_MAX))
        return 2;
    } else {
      fprintf(stderr, "unknown option %s\n", argv[i]);
      return 2;
    }
  }
  if (bus < 0 || bus > 0xffff) {
    fprintf(stderr, "--bus is required\n");
    return 2;
  }

  signature = nxinput_pad_ordinal_signature((unsigned short)bus, probe_keys);
  printf("external_bus=%d\n",
         nxinput_pad_ordinal_bus_is_external((unsigned short)bus));
  /* A assinatura SEM o gate de barramento -- a herdada pelos ports antigos.
   * Onde ela vale 1 e a assinatura completa vale 0, quem protegeu o pad foi
   * exclusivamente o gate BUS_HOST. */
  printf("signature_ungated=%d\n",
         nxinput_pad_ord_test_bit(probe_keys, BTN_GAMEPAD) &&
             (nxinput_pad_ord_test_bit(probe_keys, BTN_C) ||
              nxinput_pad_ord_test_bit(probe_keys, BTN_Z)));
  printf("signature=%d\n", signature);
  /* A/B authority: is a complete PortMaster/CFW mapping already sovereign for
   * this GUID, and does the fix defer to it? */
  sovereign = nxinput_pad_ordinal_config_has_complete_mapping(config, guid);
  printf("sovereign=%d\n", sovereign);
  printf("should_apply=%d\n",
         nxinput_pad_ordinal_should_apply(signature, sovereign, (int)force));
  if (signature &&
      nxinput_pad_ordinal_should_apply(signature, sovereign, (int)force)) {
    if (nxinput_pad_ordinal_build_mapping(mapping, sizeof(mapping), guid, name,
                                          (int)layout, probe_abs) < 0) {
      printf("mapping=OVERFLOW\n");
      return 1;
    }
    printf("mapping=%s\n", mapping);
  }
  return 0;
}
