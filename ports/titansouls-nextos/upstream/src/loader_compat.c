/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Narrow compatibility surface for untouched Titan Souls runtime shims. */

#include "loader_compat.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

typedef struct ts_exidx_view {
  uintptr_t mapping_begin;
  uintptr_t mapping_end;
  uintptr_t table;
  int entry_count;
} ts_exidx_view;

static ts_exidx_view exidx_views[TS_LOADER_MODULE_COUNT];
static int compat_bound;

/* opensles_shim.c uses these only to distinguish a guest callback address
 * from a queue-state pointer.  nxloader owns the actual mapping and lifetime. */
void *text_base;
void *data_base;
size_t text_size;
size_t data_size;

static int bind_module(ts_loader *loader, ts_loader_module_id module_id,
                       const char *anchor_export) {
  nxloader_module_info info;
  uintptr_t anchor = 0u;
  uintptr_t table = 0u;
  size_t entry_count = 0u;
  uintptr_t begin;

  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  if (ts_loader_get_info(loader, module_id, &info) != NXLOADER_OK ||
      info.arch != NXLOADER_ARCH_ARMV7 || !info.mapping_base ||
      info.mapping_size == 0u ||
      ts_loader_find_export(loader, module_id, anchor_export, &anchor) !=
          NXLOADER_OK ||
      ts_loader_find_arm_exidx(loader, module_id, anchor, &table,
                               &entry_count) != NXLOADER_OK ||
      table == 0u || entry_count == 0u || entry_count > (size_t)INT_MAX)
    return -1;

  begin = (uintptr_t)info.mapping_base;
  if (info.mapping_size > UINTPTR_MAX - begin)
    return -1;
  exidx_views[module_id].mapping_begin = begin;
  exidx_views[module_id].mapping_end = begin + info.mapping_size;
  exidx_views[module_id].table = table;
  exidx_views[module_id].entry_count = (int)entry_count;
  return 0;
}

int ts_loader_compat_bind(ts_loader *loader) {
  const ts_exidx_view *game;
  if (!loader || compat_bound)
    return -1;
  memset(exidx_views, 0, sizeof(exidx_views));
  if (bind_module(loader, TS_LOADER_MODULE_FMOD, "FMOD_System_Create") != 0 ||
      bind_module(loader, TS_LOADER_MODULE_GAME, "android_main") != 0) {
    memset(exidx_views, 0, sizeof(exidx_views));
    return -1;
  }

  game = &exidx_views[TS_LOADER_MODULE_GAME];
  text_base = (void *)game->mapping_begin;
  text_size = game->mapping_end - game->mapping_begin;
  /* These dormant diagnostics historically named the current image's whole
   * second view.  Keep them defined without manufacturing a separate map. */
  data_base = text_base;
  data_size = text_size;
  compat_bound = 1;
  return 0;
}

void ts_loader_compat_unbind(void) {
  compat_bound = 0;
  memset(exidx_views, 0, sizeof(exidx_views));
  text_base = NULL;
  data_base = NULL;
  text_size = 0u;
  data_size = 0u;
}

uintptr_t so_find_exidx(uintptr_t program_counter, int *count) {
  uintptr_t code_address = program_counter & ~(uintptr_t)1u;
  size_t index;
  if (count)
    *count = 0;
  if (!compat_bound)
    return 0u;
  for (index = 0u; index < TS_LOADER_MODULE_COUNT; ++index) {
    const ts_exidx_view *view = &exidx_views[index];
    if (code_address >= view->mapping_begin &&
        code_address < view->mapping_end) {
      if (count)
        *count = view->entry_count;
      return view->table;
    }
  }
  return 0u;
}
