/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "nxloader_internal.h"

#include <stdlib.h>
#include <string.h>

static void nxloader_registry_clear(nxloader_registry *registry) {
  size_t index;
  if (!registry)
    return;
  for (index = 0; index < registry->count; ++index) {
    free(registry->entries[index].symbol);
    free(registry->entries[index].provider);
  }
  free(registry->entries);
  registry->entries = NULL;
  registry->count = 0;
  registry->capacity = 0;
}

typedef struct nxloader_registry_candidate {
  const char *symbol;
  const char *provider;
  uintptr_t address;
  int priority;
  uint32_t flags;
  size_t sequence;
  uint8_t existing;
} nxloader_registry_candidate;

static int nxloader_registry_name_valid(const char *name) {
  return name && *name &&
         memchr(name, '\0', NXLOADER_MAX_DYNAMIC_NAME_LENGTH + 1u) != NULL;
}

static int nxloader_registry_candidate_compare(
    const nxloader_registry_candidate *left,
    const nxloader_registry_candidate *right) {
  int order = strcmp(left->symbol, right->symbol);
  if (order != 0)
    return order;
  if (left->existing != right->existing)
    return left->existing ? -1 : 1;
  if (left->sequence < right->sequence)
    return -1;
  if (left->sequence > right->sequence)
    return 1;
  return 0;
}

static void nxloader_registry_candidate_swap(
    nxloader_registry_candidate *left, nxloader_registry_candidate *right) {
  nxloader_registry_candidate temporary = *left;
  *left = *right;
  *right = temporary;
}

static void nxloader_registry_candidate_sift_down(
    nxloader_registry_candidate *items, size_t root, size_t count) {
  while (root < count / 2) {
    size_t child = root * 2 + 1;
    if (child + 1 < count &&
        nxloader_registry_candidate_compare(&items[child],
                                            &items[child + 1]) < 0)
      child++;
    if (nxloader_registry_candidate_compare(&items[root], &items[child]) >= 0)
      return;
    nxloader_registry_candidate_swap(&items[root], &items[child]);
    root = child;
  }
}

static void nxloader_registry_candidate_sort(
    nxloader_registry_candidate *items, size_t count) {
  size_t start;
  size_t end;
  if (count < 2)
    return;
  for (start = count / 2; start > 0; --start)
    nxloader_registry_candidate_sift_down(items, start - 1, count);
  for (end = count; end > 1; --end) {
    nxloader_registry_candidate_swap(&items[0], &items[end - 1]);
    nxloader_registry_candidate_sift_down(items, 0, end - 1);
  }
}

static int nxloader_registry_binary_index(const nxloader_registry *registry,
                                          const char *symbol,
                                          size_t *out_index) {
  size_t low = 0;
  size_t high = registry->count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int order = strcmp(registry->entries[middle].symbol, symbol);
    if (order < 0)
      low = middle + 1;
    else
      high = middle;
  }
  if (low >= registry->count ||
      strcmp(registry->entries[low].symbol, symbol) != 0)
    return 0;
  if (out_index)
    *out_index = low;
  return 1;
}

static int nxloader_registry_invariant(const nxloader_registry *registry) {
  size_t index;
  if (!registry || registry->count > registry->capacity ||
      (registry->count && !registry->entries))
    return 0;
  for (index = 0; index < registry->count; ++index) {
    const nxloader_registry_entry *entry = &registry->entries[index];
    if (!nxloader_registry_name_valid(entry->symbol) ||
        !nxloader_registry_name_valid(entry->provider) || !entry->address ||
        (entry->flags & ~NXLOADER_SYMBOL_WEAK) ||
        (index && strcmp(registry->entries[index - 1].symbol,
                         entry->symbol) >= 0))
      return 0;
  }
  return 1;
}

nxloader_result nxloader_registry_create(nxloader_registry **out_registry) {
  nxloader_registry *registry;
  if (!out_registry)
    return NXLOADER_EINVAL;
  *out_registry = NULL;
  registry = (nxloader_registry *)calloc(1, sizeof(*registry));
  if (!registry)
    return NXLOADER_ENOMEM;
  *out_registry = registry;
  return NXLOADER_OK;
}

void nxloader_registry_destroy(nxloader_registry *registry) {
  if (!registry)
    return;
  nxloader_registry_clear(registry);
  free(registry);
}

static int nxloader_symbol_strength(uint32_t flags) {
  return (flags & NXLOADER_SYMBOL_WEAK) ? 0 : 1;
}

nxloader_result nxloader_registry_add_provider(
    nxloader_registry *registry, const nxloader_provider *provider,
    nxloader_registry_report *report) {
  nxloader_registry_candidate *candidates = NULL;
  nxloader_registry staged;
  nxloader_registry_report local_report;
  nxloader_result result;
  size_t total;
  size_t index;
  size_t group_start;
  if (!registry || !provider || provider->struct_size < sizeof(*provider) ||
      !nxloader_registry_name_valid(provider->name) ||
      (provider->symbol_count && !provider->symbols))
    return NXLOADER_EINVAL;
  if (report && report->struct_size < sizeof(*report))
    return NXLOADER_EINVAL;
  if (!nxloader_registry_invariant(registry))
    return NXLOADER_ESTATE;
  if (provider->symbol_count > SIZE_MAX - registry->count)
    return NXLOADER_EOVERFLOW;
  total = registry->count + provider->symbol_count;
  if (total > SIZE_MAX / sizeof(*candidates) ||
      total > SIZE_MAX / sizeof(*staged.entries))
    return NXLOADER_EOVERFLOW;
  for (index = 0; index < provider->symbol_count; ++index) {
    const nxloader_symbol *symbol = &provider->symbols[index];
    if (!nxloader_registry_name_valid(symbol->name) || !symbol->address ||
        (symbol->flags & ~NXLOADER_SYMBOL_WEAK))
      return NXLOADER_EINVAL;
  }
  memset(&local_report, 0, sizeof(local_report));
  local_report.struct_size = sizeof(local_report);
  memset(&staged, 0, sizeof(staged));
  if (total) {
    candidates = (nxloader_registry_candidate *)calloc(total,
                                                        sizeof(*candidates));
    staged.entries = (nxloader_registry_entry *)calloc(
        total, sizeof(*staged.entries));
    if (!candidates || !staged.entries) {
      result = NXLOADER_ENOMEM;
      goto fail;
    }
    staged.capacity = total;
  }
  for (index = 0; index < registry->count; ++index) {
    const nxloader_registry_entry *entry = &registry->entries[index];
    candidates[index].symbol = entry->symbol;
    candidates[index].provider = entry->provider;
    candidates[index].address = entry->address;
    candidates[index].priority = entry->priority;
    candidates[index].flags = entry->flags;
    candidates[index].sequence = index;
    candidates[index].existing = 1;
  }
  for (index = 0; index < provider->symbol_count; ++index) {
    const nxloader_symbol *symbol = &provider->symbols[index];
    nxloader_registry_candidate *candidate =
        &candidates[registry->count + index];
    candidate->symbol = symbol->name;
    candidate->provider = provider->name;
    candidate->address = symbol->address;
    candidate->priority = provider->priority;
    candidate->flags = symbol->flags;
    candidate->sequence = index;
  }
  nxloader_registry_candidate_sort(candidates, total);
  for (group_start = 0; group_start < total;) {
    const nxloader_registry_candidate *winner = &candidates[group_start];
    nxloader_registry_entry *entry;
    size_t group_end = group_start + 1;
    if (!winner->existing)
      local_report.added++;
    while (group_end < total &&
           strcmp(candidates[group_end].symbol, winner->symbol) == 0) {
      const nxloader_registry_candidate *candidate = &candidates[group_end];
      int new_strength;
      int old_strength;
      int order;
      if (candidate->existing) {
        result = NXLOADER_ESTATE;
        goto fail;
      }
      new_strength = nxloader_symbol_strength(candidate->flags);
      old_strength = nxloader_symbol_strength(winner->flags);
      order = candidate->priority == winner->priority
                  ? new_strength - old_strength
                  : (candidate->priority > winner->priority ? 1 : -1);
      if (order == 0) {
        if (winner->address != candidate->address) {
          result = NXLOADER_ECOLLISION;
          goto fail;
        }
        local_report.equivalent++;
      } else if (order < 0) {
        local_report.ignored_lower_priority++;
      } else {
        winner = candidate;
        local_report.replaced_lower_priority++;
      }
      group_end++;
    }
    entry = &staged.entries[staged.count];
    entry->symbol = nxloader_strdup(winner->symbol);
    entry->provider = nxloader_strdup(winner->provider);
    if (!entry->symbol || !entry->provider) {
      free(entry->symbol);
      free(entry->provider);
      memset(entry, 0, sizeof(*entry));
      result = NXLOADER_ENOMEM;
      goto fail;
    }
    entry->address = winner->address;
    entry->priority = winner->priority;
    entry->flags = winner->flags;
    staged.count++;
    group_start = group_end;
  }
  free(candidates);
  nxloader_registry_clear(registry);
  *registry = staged;
  if (report) {
    size_t struct_size = report->struct_size;
    *report = local_report;
    report->struct_size = struct_size;
  }
  return NXLOADER_OK;

fail:
  free(candidates);
  nxloader_registry_clear(&staged);
  return result;
}

nxloader_result nxloader_registry_lookup(const nxloader_registry *registry,
                                         const char *symbol,
                                         nxloader_registry_match *match) {
  size_t index;
  size_t struct_size;
  if (!registry || !nxloader_registry_name_valid(symbol) || !match ||
      match->struct_size < sizeof(*match))
    return NXLOADER_EINVAL;
  if (registry->count > registry->capacity ||
      (registry->count && !registry->entries))
    return NXLOADER_ESTATE;
  if (!nxloader_registry_binary_index(registry, symbol, &index))
    return NXLOADER_EUNRESOLVED;
  struct_size = match->struct_size;
  memset(match, 0, sizeof(*match));
  match->struct_size = struct_size;
  match->address = registry->entries[index].address;
  match->priority = registry->entries[index].priority;
  match->flags = registry->entries[index].flags;
  match->provider = registry->entries[index].provider;
  return NXLOADER_OK;
}

nxloader_result nxloader_registry_add_module(nxloader_registry *registry,
                                             const nxloader_module *module,
                                             const char *provider_name,
                                             int priority,
                                             nxloader_registry_report *report) {
  if (!module)
    return NXLOADER_EINVAL;
  if (nxloader_module_callback_guard(module) != NXLOADER_OK)
    return NXLOADER_EREENTRANT;
  if (!registry || !nxloader_registry_name_valid(provider_name))
    return NXLOADER_EINVAL;
  if (module->state != NXLOADER_STATE_RELOCATED &&
      module->state != NXLOADER_STATE_RESOLVED &&
      module->state != NXLOADER_STATE_FINALIZED &&
      module->state != NXLOADER_STATE_INITIALIZED &&
      module->state != NXLOADER_STATE_READY)
    return NXLOADER_ESTATE;
  return module->arch == NXLOADER_ARCH_ARMV7
             ? nxloader_add_exports_elf32(registry, module, provider_name,
                                          priority, report)
             : nxloader_add_exports_elf64(registry, module, provider_name,
                                          priority, report);
}
