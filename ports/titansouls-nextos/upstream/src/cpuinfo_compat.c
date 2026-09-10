#include "cpuinfo_compat.h"

#include <ctype.h>
#include <string.h>

static const char *find_bytes(const char *buffer, size_t length,
                              const char *needle) {
  size_t needle_length;
  size_t offset;

  if (!buffer || !needle)
    return NULL;
  needle_length = strlen(needle);
  if (needle_length == 0u || needle_length > length)
    return NULL;
  for (offset = 0u; offset + needle_length <= length; ++offset) {
    if (memcmp(buffer + offset, needle, needle_length) == 0)
      return buffer + offset;
  }
  return NULL;
}

static const char *line_end(const char *line, const char *limit) {
  const char *cursor = line;
  while (cursor < limit && *cursor != '\n' && *cursor != '\r')
    ++cursor;
  return cursor;
}

static int line_has_token(const char *line, const char *end,
                          const char *token) {
  size_t token_length = strlen(token);
  const char *cursor = line;

  while (cursor < end) {
    const char *start;
    while (cursor < end && isspace((unsigned char)*cursor))
      ++cursor;
    start = cursor;
    while (cursor < end && !isspace((unsigned char)*cursor))
      ++cursor;
    if ((size_t)(cursor - start) == token_length &&
        memcmp(start, token, token_length) == 0)
      return 1;
  }
  return 0;
}

static int cpu_architecture_is_aarch64(const char *buffer, size_t length) {
  static const char label[] = "CPU architecture";
  const char *found = find_bytes(buffer, length, label);
  const char *limit = buffer + length;
  const char *cursor;
  unsigned int architecture = 0u;

  if (!found)
    return 0;
  cursor = found + sizeof(label) - 1u;
  while (cursor < limit && (*cursor == ' ' || *cursor == '\t' ||
                            *cursor == ':'))
    ++cursor;
  if (cursor >= limit || !isdigit((unsigned char)*cursor))
    return 0;
  while (cursor < limit && isdigit((unsigned char)*cursor)) {
    architecture = architecture * 10u + (unsigned int)(*cursor - '0');
    ++cursor;
  }
  return architecture >= 8u;
}

int ts_cpuinfo_add_armv7_aliases(char *buffer, size_t *length,
                                 size_t capacity) {
  static const char features_label[] = "Features";
  static const char vfp_aliases[] = " vfp vfpv3 vfpv3d16";
  static const char neon_alias[] = " neon";
  char aliases[sizeof(vfp_aliases) + sizeof(neon_alias)];
  const char *found;
  const char *end;
  size_t input_length;
  size_t aliases_length = 0u;
  size_t insert_offset;
  size_t tail_length;
  size_t tail_capacity;
  size_t tail_to_keep;

  if (!buffer || !length || *length > capacity)
    return -1;
  input_length = *length;
  if (!cpu_architecture_is_aarch64(buffer, input_length))
    return 0;

  found = find_bytes(buffer, input_length, features_label);
  if (!found)
    return 0;
  end = line_end(found, buffer + input_length);

  if (line_has_token(found, end, "fp")) {
    int has_vfp = line_has_token(found, end, "vfp");
    int has_vfpv3 = line_has_token(found, end, "vfpv3");
    int has_vfpv3d16 = line_has_token(found, end, "vfpv3d16");
    if (!has_vfp || !has_vfpv3 || !has_vfpv3d16) {
      memcpy(aliases + aliases_length, vfp_aliases,
             sizeof(vfp_aliases) - 1u);
      aliases_length += sizeof(vfp_aliases) - 1u;
    }
  }
  if (line_has_token(found, end, "asimd") &&
      !line_has_token(found, end, "neon")) {
    memcpy(aliases + aliases_length, neon_alias, sizeof(neon_alias) - 1u);
    aliases_length += sizeof(neon_alias) - 1u;
  }
  if (aliases_length == 0u)
    return 0;

  insert_offset = (size_t)(end - buffer);
  if (insert_offset + aliases_length > capacity)
    return 0;
  tail_length = input_length - insert_offset;
  tail_capacity = capacity - insert_offset - aliases_length;
  tail_to_keep = tail_length < tail_capacity ? tail_length : tail_capacity;
  memmove(buffer + insert_offset + aliases_length, buffer + insert_offset,
          tail_to_keep);
  memcpy(buffer + insert_offset, aliases, aliases_length);
  *length = insert_offset + aliases_length + tail_to_keep;
  return 1;
}
