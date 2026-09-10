/* SPDX-License-Identifier: GPL-3.0-only */
/* Native language-menu completion for Titan Souls Android 1.0.3. */

#define _GNU_SOURCE
#include "language_menu.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <nxloader_softfp.h>

#include "language_menu_policy.h"
#include "util.h"

#define TS_OPTIONS_LANGUAGE_OFFSET 0xe4u
#define TS_OPTIONS_VECTOR_BEGIN_OFFSET 0x94u
#define TS_OPTIONS_VECTOR_END_OFFSET 0x98u
#define TS_OPTION_HEIGHT_OFFSET 0x9cu
#define TS_TOGGLE_CHOICES_OFFSET 0xbcu
#define TS_TOGGLE_SELECTED_OFFSET 0xc8u

#define TS_GUEST_STRING_CTOR_OFFSET 0x301a00u
#define TS_GUEST_STRING_DTOR_OFFSET 0x300880u

#define TS_SYM_OPTIONS_ADDED "_ZN11OptionsMenu5AddedEv"
#define TS_SYM_OPTIONS_APPLY_LANGUAGE "_ZN11OptionsMenu13ApplyLanguageEv"
#define TS_SYM_SYSTEM_LANGUAGE "_ZN8AgLocale24getCurrentSystemLanguageEv"
#define TS_SYM_UIMENU_ADDED "_ZN6UIMenu5AddedEv"
#define TS_SYM_LANGUAGE_CHANGE "_ZN8Language14ChangeLanguageESs"
#define TS_SYM_CONFIG_SAVE "_ZN10ConfigSave4SaveEv"
#define TS_SYM_GAME_CONFIG "_ZN4GAME6configE"
#define TS_SYM_VECTOR_EMPLACE \
  "_ZNSt6vectorISsSaISsEE12emplace_backIISsEEEvDpOT_"

typedef struct ts_guest_string {
  char *data;
} ts_guest_string;

typedef struct ts_guest_string_vector {
  ts_guest_string *begin;
  ts_guest_string *end;
  ts_guest_string *capacity;
} ts_guest_string_vector;

typedef void *NXLOADER_ARM_SOFTFP (*ts_string_ctor_fn)(
    ts_guest_string *, const char *, void *);
typedef void NXLOADER_ARM_SOFTFP (*ts_string_dtor_fn)(ts_guest_string *);
typedef void NXLOADER_ARM_SOFTFP (*ts_vector_emplace_fn)(
    ts_guest_string_vector *, ts_guest_string *);
typedef void NXLOADER_ARM_SOFTFP (*ts_guest_self_fn)(void *);
typedef void NXLOADER_ARM_SOFTFP (*ts_language_change_fn)(ts_guest_string *);

static ts_string_ctor_fn g_string_ctor;
static ts_string_dtor_fn g_string_dtor;
static ts_vector_emplace_fn g_vector_emplace;
static ts_guest_self_fn g_uimenu_added;
static ts_language_change_fn g_language_change;
static ts_guest_self_fn g_config_save;
static void **g_game_config;

static char g_config_path[PATH_MAX];
static int g_selected_index;
static int g_system_locale = 4;
static int g_locale = 4;

static const char *host_locale_name(void) {
  const char *value = getenv("LC_ALL");
  if (!value || !value[0])
    value = getenv("LC_MESSAGES");
  if (!value || !value[0])
    value = getenv("LANG");
  return value;
}

static int extract_config_language(const char *path, char *value,
                                   size_t value_size) {
  struct stat info;
  FILE *input = NULL;
  char *contents = NULL;
  char *tag;
  char *tag_end;
  char *start;
  char *end;
  long length;
  size_t value_length;
  int status = -1;

  if (!path || !value || value_size < 2u || lstat(path, &info) != 0 ||
      !S_ISREG(info.st_mode) || info.st_size < 1 || info.st_size > 1024 * 1024)
    return -1;
  input = fopen(path, "rb");
  if (!input || fseek(input, 0, SEEK_END) != 0 ||
      (length = ftell(input)) < 1 || fseek(input, 0, SEEK_SET) != 0)
    goto done;
  contents = (char *)malloc((size_t)length + 1u);
  if (!contents || fread(contents, 1, (size_t)length, input) != (size_t)length)
    goto done;
  contents[length] = '\0';
  tag = strstr(contents, "<language");
  tag_end = tag ? strchr(tag, '>') : NULL;
  start = tag ? strstr(tag, "lang=\"") : NULL;
  if (!tag_end || !start || start >= tag_end)
    goto done;
  start += strlen("lang=\"");
  end = strchr(start, '"');
  if (!end || end > tag_end)
    goto done;
  value_length = (size_t)(end - start);
  if (value_length == 0u || value_length >= value_size)
    goto done;
  memcpy(value, start, value_length);
  value[value_length] = '\0';
  status = 0;

done:
  if (input)
    fclose(input);
  free(contents);
  return status;
}

int ts_language_menu_prepare(const char *gamedir) {
  char engine_key[32];
  const ts_language_choice *choice;
  int index;
  if (!gamedir || !gamedir[0] ||
      snprintf(g_config_path, sizeof(g_config_path),
               "%s/sdcard/TitanSoulsSave/data/config.txt", gamedir) >=
          (int)sizeof(g_config_path))
    return -1;

  g_system_locale = ts_language_system_locale(host_locale_name());
  if (extract_config_language(g_config_path, engine_key, sizeof(engine_key)) !=
      0) {
    logPrintf("language-menu: config sem idioma valido; usando sistema\n");
    g_selected_index = 0;
    g_locale = g_system_locale;
    return 0;
  }
  index = ts_language_choice_index(engine_key);
  if (index < 0) {
    logPrintf("language-menu: chave desconhecida '%s'; usando sistema\n",
              engine_key);
    g_selected_index = 0;
    g_locale = g_system_locale;
    return 0;
  }
  choice = ts_language_choice_at((size_t)index);
  g_selected_index = index;
  g_locale = index == 0 ? g_system_locale : choice->ag_locale;
  logPrintf("language-menu: escolha persistida=%s indice=%d locale=%d\n",
            engine_key, g_selected_index, g_locale);
  return 0;
}

static int vector_count(const ts_guest_string_vector *vector,
                        size_t *count) {
  uintptr_t begin;
  uintptr_t end;
  if (!vector || !count)
    return -1;
  begin = (uintptr_t)vector->begin;
  end = (uintptr_t)vector->end;
  if ((!begin && end) || end < begin ||
      (end - begin) % sizeof(ts_guest_string) != 0u)
    return -1;
  *count = begin ? (end - begin) / sizeof(ts_guest_string) : 0u;
  return 0;
}

static int append_choice(ts_guest_string_vector *vector, const char *label) {
  ts_guest_string value;
  unsigned char allocator = 0u;
  memset(&value, 0, sizeof(value));
  if (!vector || !label || !g_string_ctor || !g_string_dtor ||
      !g_vector_emplace)
    return -1;
  g_string_ctor(&value, label, &allocator);
  if (!value.data)
    return -1;
  g_vector_emplace(vector, &value);
  g_string_dtor(&value);
  return 0;
}

static void complete_language_choices(void *options_menu) {
  void *toggle;
  ts_guest_string_vector *choices;
  ts_guest_string titan;
  size_t count;
  size_t index;
  if (!options_menu)
    return;
  toggle = *(void **)((unsigned char *)options_menu +
                      TS_OPTIONS_LANGUAGE_OFFSET);
  if (!toggle)
    return;
  choices = (ts_guest_string_vector *)((unsigned char *)toggle +
                                       TS_TOGGLE_CHOICES_OFFSET);
  if (vector_count(choices, &count) != 0) {
    logPrintf("language-menu: vetor de escolhas invalido\n");
    return;
  }
  if (count == 2u) {
    /* Android entrega [system,titan]. Acrescentamos os cinco idiomas reais e
     * movemos titan para o final sem reconstruir/destruir objetos do guest. */
    for (index = 1u; index + 1u < ts_language_choice_count(); ++index) {
      const ts_language_choice *choice = ts_language_choice_at(index);
      if (!choice || append_choice(choices, choice->menu_label) != 0) {
        logPrintf("language-menu: falha ao acrescentar escolha %lu\n",
                  (unsigned long)index);
        return;
      }
    }
    if (vector_count(choices, &count) != 0 ||
        count != ts_language_choice_count()) {
      logPrintf("language-menu: contagem final inesperada (%lu)\n",
                (unsigned long)count);
      return;
    }
    titan = choices->begin[1];
    memmove(&choices->begin[1], &choices->begin[2],
            (count - 2u) * sizeof(ts_guest_string));
    choices->begin[count - 1u] = titan;
    logPrintf("language-menu: menu nativo expandido para %lu escolhas\n",
              (unsigned long)count);
  }
  if (count == ts_language_choice_count())
    *(int *)((unsigned char *)toggle + TS_TOGGLE_SELECTED_OFFSET) =
        g_selected_index;
}

static void NXLOADER_ARM_SOFTFP ts_options_added(void *options_menu) {
  void **begin;
  void **end;
  if (g_uimenu_added)
    g_uimenu_added(options_menu);
  if (!options_menu)
    return;
  complete_language_choices(options_menu);
  begin = *(void ***)((unsigned char *)options_menu +
                     TS_OPTIONS_VECTOR_BEGIN_OFFSET);
  end = *(void ***)((unsigned char *)options_menu +
                   TS_OPTIONS_VECTOR_END_OFFSET);
  while (begin && end && begin < end) {
    if (*begin)
      *(float *)((unsigned char *)*begin + TS_OPTION_HEIGHT_OFFSET) = 0.5f;
    ++begin;
  }
}

static void NXLOADER_ARM_SOFTFP ts_apply_language(void *options_menu) {
  void *toggle;
  ts_guest_string_vector *choices;
  ts_guest_string requested;
  unsigned char allocator = 0u;
  const ts_language_choice *choice;
  size_t count;
  int selected;
  if (!options_menu || !g_string_ctor || !g_string_dtor ||
      !g_language_change)
    return;
  toggle = *(void **)((unsigned char *)options_menu +
                      TS_OPTIONS_LANGUAGE_OFFSET);
  if (!toggle)
    return;
  choices = (ts_guest_string_vector *)((unsigned char *)toggle +
                                       TS_TOGGLE_CHOICES_OFFSET);
  selected = *(int *)((unsigned char *)toggle + TS_TOGGLE_SELECTED_OFFSET);
  if (vector_count(choices, &count) != 0 ||
      count != ts_language_choice_count() || selected < 0 ||
      (size_t)selected >= count ||
      (size_t)selected >= ts_language_choice_count()) {
    logPrintf("language-menu: selecao invalida %d/%lu recusada\n", selected,
              (unsigned long)count);
    return;
  }
  choice = ts_language_choice_at((size_t)selected);
  g_selected_index = selected;
  g_locale = selected == 0 ? g_system_locale : choice->ag_locale;
  memset(&requested, 0, sizeof(requested));
  g_string_ctor(&requested,
                selected + 1 == (int)ts_language_choice_count()
                    ? "titan"
                    : "systemlang",
                &allocator);
  if (!requested.data) {
    logPrintf("language-menu: construcao da chave %s falhou\n",
              choice->engine_key);
    return;
  }
  g_language_change(&requested);
  g_string_dtor(&requested);
  if (g_config_save && g_game_config && *g_game_config)
    g_config_save(*g_game_config);
  logPrintf("language-menu: aplicado %s (locale=%d)\n", choice->engine_key,
            g_locale);
}

static int NXLOADER_ARM_SOFTFP ts_current_system_language(void) {
  return g_locale;
}

static int bind_guest(ts_loader *loader, const char *symbol,
                      void *destination, size_t destination_size) {
  uintptr_t address = 0u;
  if (!loader || !symbol || !destination ||
      destination_size != sizeof(address) ||
      ts_loader_find_export(loader, TS_LOADER_MODULE_GAME, symbol, &address) !=
          NXLOADER_OK ||
      address == 0u)
    return -1;
  memcpy(destination, &address, destination_size);
  return 0;
}

static int entry_matches(uintptr_t address, const unsigned char expected[8]) {
  return address &&
         memcmp((const void *)(address & ~(uintptr_t)1u), expected, 8u) == 0;
}

int ts_language_menu_install(ts_loader *loader, uintptr_t mapping_base,
                             size_t mapping_size) {
  static const unsigned char added_entry[8] = {
      0x10, 0xb5, 0x04, 0x46, 0x03, 0xf0, 0xe1, 0xfe,
  };
  static const unsigned char apply_entry[8] = {
      0x37, 0xb5, 0xd0, 0xf8, 0x8c, 0x20, 0xd0, 0xf8,
  };
  static const unsigned char locale_entry[8] = {
      0x53, 0x4b, 0x37, 0xb5, 0x7b, 0x44, 0x1b, 0x68,
  };
  static const unsigned char string_ctor_entry[8] = {
      0x70, 0xb5, 0x0c, 0x46, 0x05, 0x46, 0x16, 0x46,
  };
  static const unsigned char string_dtor_entry[8] = {
      0x10, 0xb5, 0x04, 0x46, 0x06, 0x4b, 0x82, 0xb0,
  };
  uintptr_t added = 0u;
  uintptr_t apply = 0u;
  uintptr_t locale = 0u;
  uintptr_t hidden_ctor;
  uintptr_t hidden_dtor;
  nxloader_result result;

  if (!loader || !mapping_base ||
      mapping_size <= TS_GUEST_STRING_CTOR_OFFSET + 8u)
    return -1;
  if (ts_loader_find_export(loader, TS_LOADER_MODULE_GAME,
                            TS_SYM_OPTIONS_ADDED, &added) != NXLOADER_OK ||
      ts_loader_find_export(loader, TS_LOADER_MODULE_GAME,
                            TS_SYM_OPTIONS_APPLY_LANGUAGE, &apply) !=
          NXLOADER_OK ||
      ts_loader_find_export(loader, TS_LOADER_MODULE_GAME,
                            TS_SYM_SYSTEM_LANGUAGE, &locale) != NXLOADER_OK ||
      bind_guest(loader, TS_SYM_UIMENU_ADDED, &g_uimenu_added,
                 sizeof(g_uimenu_added)) != 0 ||
      bind_guest(loader, TS_SYM_LANGUAGE_CHANGE, &g_language_change,
                 sizeof(g_language_change)) != 0 ||
      bind_guest(loader, TS_SYM_CONFIG_SAVE, &g_config_save,
                 sizeof(g_config_save)) != 0 ||
      bind_guest(loader, TS_SYM_GAME_CONFIG, &g_game_config,
                 sizeof(g_game_config)) != 0 ||
      bind_guest(loader, TS_SYM_VECTOR_EMPLACE, &g_vector_emplace,
                 sizeof(g_vector_emplace)) != 0)
    return -1;

  hidden_ctor = mapping_base + TS_GUEST_STRING_CTOR_OFFSET;
  hidden_dtor = mapping_base + TS_GUEST_STRING_DTOR_OFFSET;
  if (!entry_matches(added, added_entry) ||
      !entry_matches(apply, apply_entry) ||
      !entry_matches(locale, locale_entry) ||
      !entry_matches(hidden_ctor, string_ctor_entry) ||
      !entry_matches(hidden_dtor, string_dtor_entry)) {
    logPrintf("language-menu: assinatura do guest divergiu; hooks recusados\n");
    return -1;
  }
  hidden_ctor |= 1u;
  hidden_dtor |= 1u;
  memcpy(&g_string_ctor, &hidden_ctor, sizeof(g_string_ctor));
  memcpy(&g_string_dtor, &hidden_dtor, sizeof(g_string_dtor));

  result = ts_loader_install_hook(loader, TS_LOADER_MODULE_GAME, added,
                                  (uintptr_t)&ts_options_added, 8u);
  if (result != NXLOADER_OK)
    return -1;
  result = ts_loader_install_hook(loader, TS_LOADER_MODULE_GAME, apply,
                                  (uintptr_t)&ts_apply_language, 8u);
  if (result != NXLOADER_OK)
    return -1;
  result = ts_loader_install_hook(loader, TS_LOADER_MODULE_GAME, locale,
                                  (uintptr_t)&ts_current_system_language, 8u);
  if (result != NXLOADER_OK)
    return -1;
  logPrintf("language-menu: hooks nativos instalados (system+5+titan)\n");
  return 0;
}
