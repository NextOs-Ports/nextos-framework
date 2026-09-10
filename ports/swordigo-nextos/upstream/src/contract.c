/* SPDX-License-Identifier: GPL-3.0-only */
#include "contract.h"

#include <stdlib.h>
#include <string.h>

int swordigo_contract_has_token(const char *list, const char *token) {
  size_t wanted;
  const char *cursor;

  if (!list || !token || !token[0])
    return 0;
  wanted = strlen(token);
  cursor = list;
  while (*cursor) {
    const char *end = strchr(cursor, '\n');
    size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
    if (length && cursor[length - 1] == '\r')
      --length;
    if (length == wanted && memcmp(cursor, token, wanted) == 0)
      return 1;
    if (!end)
      break;
    cursor = end + 1;
  }
  return 0;
}

int swordigo_contract_quirk_enabled(const char *token) {
  return swordigo_contract_has_token(getenv("NXCOMPAT_ENABLED_QUIRKS"), token);
}
