/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxandroid.h"

#include <stdlib.h>
#include <string.h>

typedef struct nxandroid_owned_catalog_entry {
  nxandroid_catalog_entry entry;
  char *name;
  char *contract_id;
  char *stub_semantics;
} nxandroid_owned_catalog_entry;

struct nxandroid_import_catalog {
  nxandroid_owned_catalog_entry *entries;
  size_t entry_count;
};

static int nxandroid_bounded_length(const char *text, size_t maximum,
                                    size_t *length) {
  size_t index;
  if (text == NULL)
    return 0;
  for (index = 0u; index <= maximum; ++index) {
    if (text[index] == '\0') {
      if (length != NULL)
        *length = index;
      return index != 0u;
    }
  }
  return 0;
}

static char *nxandroid_copy_string(const char *text, size_t maximum) {
  size_t size;
  size_t length;
  char *copy;
  if (text == NULL)
    return NULL;
  if (!nxandroid_bounded_length(text, maximum, &length))
    return NULL;
  size = length + 1u;
  copy = (char *)malloc(size);
  if (copy != NULL)
    memcpy(copy, text, size);
  return copy;
}

static int nxandroid_valid_abi(nxandroid_abi abi) {
  return abi >= NXANDROID_ABI_ARMV7_BIONIC &&
         abi <= NXANDROID_ABI_X86_64_BIONIC;
}

static int nxandroid_valid_domain(nxandroid_import_domain domain) {
  return domain >= NXANDROID_IMPORT_BIONIC && domain <= NXANDROID_IMPORT_NDK;
}

static int nxandroid_valid_symbol_kind(nxandroid_symbol_kind kind) {
  return kind == NXANDROID_SYMBOL_FUNCTION || kind == NXANDROID_SYMBOL_DATA;
}

static int
nxandroid_valid_criticality(nxandroid_import_criticality criticality) {
  return criticality == NXANDROID_IMPORT_OPTIONAL ||
         criticality == NXANDROID_IMPORT_CRITICAL;
}

static int nxandroid_valid_catalog_entry(
    const nxandroid_catalog_entry *entry) {
  if (entry == NULL ||
      !nxandroid_bounded_length(entry->name, NXANDROID_SYMBOL_NAME_MAX, NULL) ||
      !nxandroid_valid_abi(entry->abi) ||
      !nxandroid_valid_domain(entry->domain) ||
      !nxandroid_valid_symbol_kind(entry->symbol_kind) ||
      !nxandroid_valid_criticality(entry->criticality) ||
      !nxandroid_bounded_length(entry->contract_id,
                                NXANDROID_CONTRACT_ID_MAX, NULL) ||
      entry->address == 0u ||
      (entry->provider_kind != NXANDROID_PROVIDER_IMPLEMENTATION &&
       entry->provider_kind != NXANDROID_PROVIDER_STUB))
    return 0;
  if (entry->provider_kind == NXANDROID_PROVIDER_STUB)
    return nxandroid_bounded_length(entry->stub_semantics,
                                    NXANDROID_STUB_SEMANTICS_MAX, NULL);
  return entry->stub_semantics == NULL;
}

static int nxandroid_compare_key(const nxandroid_catalog_entry *left,
                                 const nxandroid_catalog_entry *right) {
  int text_result;
  if (left->abi != right->abi)
    return left->abi < right->abi ? -1 : 1;
  if (left->domain != right->domain)
    return left->domain < right->domain ? -1 : 1;
  text_result = strcmp(left->name, right->name);
  if (text_result != 0)
    return text_result;
  if (left->symbol_kind != right->symbol_kind)
    return left->symbol_kind < right->symbol_kind ? -1 : 1;
  if (left->criticality != right->criticality)
    return left->criticality < right->criticality ? -1 : 1;
  return strcmp(left->contract_id, right->contract_id);
}

static void nxandroid_merge_entries(nxandroid_owned_catalog_entry *entries,
                                    nxandroid_owned_catalog_entry *scratch,
                                    size_t begin, size_t middle, size_t end) {
  size_t left = begin;
  size_t right = middle;
  size_t output = begin;
  while (left < middle && right < end) {
    if (nxandroid_compare_key(&entries[left].entry,
                              &entries[right].entry) <= 0)
      scratch[output++] = entries[left++];
    else
      scratch[output++] = entries[right++];
  }
  while (left < middle)
    scratch[output++] = entries[left++];
  while (right < end)
    scratch[output++] = entries[right++];
  for (output = begin; output < end; ++output)
    entries[output] = scratch[output];
}

static void nxandroid_sort_entries(nxandroid_owned_catalog_entry *entries,
                                   nxandroid_owned_catalog_entry *scratch,
                                   size_t begin, size_t end) {
  size_t middle;
  if (end - begin < 2u)
    return;
  middle = begin + (end - begin) / 2u;
  nxandroid_sort_entries(entries, scratch, begin, middle);
  nxandroid_sort_entries(entries, scratch, middle, end);
  nxandroid_merge_entries(entries, scratch, begin, middle, end);
}

static void nxandroid_free_owned_entry(nxandroid_owned_catalog_entry *entry) {
  if (entry == NULL)
    return;
  free(entry->name);
  free(entry->contract_id);
  free(entry->stub_semantics);
}

nxandroid_result
nxandroid_import_catalog_create(const nxandroid_catalog_entry *entries,
                                size_t entry_count,
                                nxandroid_import_catalog **output) {
  nxandroid_import_catalog *catalog;
  nxandroid_owned_catalog_entry *scratch;
  size_t index;

  if (output == NULL)
    return NXANDROID_EINVAL;
  *output = NULL;
  if (entries == NULL || entry_count == 0u ||
      entry_count > NXANDROID_MAX_CATALOG_ENTRIES)
    return NXANDROID_EINVAL;

  catalog = (nxandroid_import_catalog *)calloc(1u, sizeof(*catalog));
  if (catalog == NULL)
    return NXANDROID_ENOMEM;
  catalog->entries = (nxandroid_owned_catalog_entry *)calloc(
      entry_count, sizeof(*catalog->entries));
  if (catalog->entries == NULL) {
    free(catalog);
    return NXANDROID_ENOMEM;
  }
  catalog->entry_count = entry_count;

  for (index = 0; index < entry_count; ++index) {
    nxandroid_owned_catalog_entry *owned = &catalog->entries[index];
    if (!nxandroid_valid_catalog_entry(&entries[index])) {
      nxandroid_import_catalog_destroy(&catalog);
      return NXANDROID_ECATALOG;
    }
    owned->entry = entries[index];
    owned->name =
        nxandroid_copy_string(entries[index].name, NXANDROID_SYMBOL_NAME_MAX);
    owned->contract_id = nxandroid_copy_string(entries[index].contract_id,
                                                NXANDROID_CONTRACT_ID_MAX);
    if (entries[index].stub_semantics != NULL)
      owned->stub_semantics =
          nxandroid_copy_string(entries[index].stub_semantics,
                                NXANDROID_STUB_SEMANTICS_MAX);
    if (owned->name == NULL || owned->contract_id == NULL ||
        (entries[index].stub_semantics != NULL &&
         owned->stub_semantics == NULL)) {
      nxandroid_import_catalog_destroy(&catalog);
      return NXANDROID_ENOMEM;
    }
    owned->entry.name = owned->name;
    owned->entry.contract_id = owned->contract_id;
    owned->entry.stub_semantics = owned->stub_semantics;
  }

  scratch = (nxandroid_owned_catalog_entry *)calloc(
      catalog->entry_count, sizeof(*scratch));
  if (scratch == NULL) {
    nxandroid_import_catalog_destroy(&catalog);
    return NXANDROID_ENOMEM;
  }
  nxandroid_sort_entries(catalog->entries, scratch, 0u, catalog->entry_count);
  free(scratch);
  for (index = 1u; index < catalog->entry_count; ++index) {
    if (nxandroid_compare_key(&catalog->entries[index - 1u].entry,
                              &catalog->entries[index].entry) == 0) {
      nxandroid_import_catalog_destroy(&catalog);
      return NXANDROID_ECATALOG;
    }
  }

  *output = catalog;
  return NXANDROID_OK;
}

void nxandroid_import_catalog_destroy(nxandroid_import_catalog **catalog) {
  size_t index;
  if (catalog == NULL || *catalog == NULL)
    return;
  for (index = 0; index < (*catalog)->entry_count; ++index)
    nxandroid_free_owned_entry(&(*catalog)->entries[index]);
  free((*catalog)->entries);
  free(*catalog);
  *catalog = NULL;
}

static int nxandroid_valid_request(const nxandroid_import_request *request) {
  return request != NULL &&
         nxandroid_bounded_length(request->name, NXANDROID_SYMBOL_NAME_MAX,
                                  NULL) &&
         nxandroid_valid_abi(request->abi) &&
         nxandroid_valid_domain(request->domain) &&
         nxandroid_valid_symbol_kind(request->symbol_kind) &&
         nxandroid_valid_criticality(request->criticality) &&
         (request->binding == NXANDROID_BIND_STRONG ||
          request->binding == NXANDROID_BIND_WEAK) &&
         nxandroid_bounded_length(request->contract_id,
                                  NXANDROID_CONTRACT_ID_MAX, NULL) &&
         (request->allow_stub == 0 || request->allow_stub == 1);
}

static int nxandroid_compare_request_entry(
    const nxandroid_import_request *request,
    const nxandroid_catalog_entry *entry) {
  nxandroid_catalog_entry key;
  memset(&key, 0, sizeof(key));
  key.name = request->name;
  key.abi = request->abi;
  key.domain = request->domain;
  key.symbol_kind = request->symbol_kind;
  key.criticality = request->criticality;
  key.contract_id = request->contract_id;
  return nxandroid_compare_key(&key, entry);
}

static const nxandroid_owned_catalog_entry *nxandroid_find_entry(
    const nxandroid_import_catalog *catalog,
    const nxandroid_import_request *request) {
  size_t low = 0u;
  size_t high = catalog->entry_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2u;
    int comparison =
        nxandroid_compare_request_entry(request, &catalog->entries[middle].entry);
    if (comparison == 0)
      return &catalog->entries[middle];
    if (comparison < 0)
      high = middle;
    else
      low = middle + 1u;
  }
  return NULL;
}

static int nxandroid_compare_request_prefix(
    const nxandroid_import_request *request,
    const nxandroid_catalog_entry *entry) {
  if (request->abi != entry->abi)
    return request->abi < entry->abi ? -1 : 1;
  if (request->domain != entry->domain)
    return request->domain < entry->domain ? -1 : 1;
  return strcmp(request->name, entry->name);
}

static int nxandroid_has_same_named_contract(
    const nxandroid_import_catalog *catalog,
    const nxandroid_import_request *request) {
  size_t low = 0u;
  size_t high = catalog->entry_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2u;
    int comparison = nxandroid_compare_request_prefix(
        request, &catalog->entries[middle].entry);
    if (comparison <= 0)
      high = middle;
    else
      low = middle + 1u;
  }
  return low < catalog->entry_count &&
         nxandroid_compare_request_prefix(
             request, &catalog->entries[low].entry) == 0;
}

nxandroid_result nxandroid_imports_resolve(
    const nxandroid_import_catalog *catalog,
    const nxandroid_import_request *requests, size_t request_count,
    nxandroid_import_binding_result *bindings, size_t *bad_request) {
  nxandroid_import_binding_result *temporary;
  nxandroid_result result = NXANDROID_OK;
  size_t index;

  if (bad_request != NULL)
    *bad_request = NXANDROID_NO_MODULE;
  if (catalog == NULL || requests == NULL || request_count == 0u ||
      request_count > NXANDROID_MAX_IMPORT_REQUESTS || bindings == NULL)
    return NXANDROID_EINVAL;
  temporary = (nxandroid_import_binding_result *)calloc(
      request_count, sizeof(*temporary));
  if (temporary == NULL)
    return NXANDROID_ENOMEM;

  for (index = 0; index < request_count; ++index) {
    const nxandroid_import_request *request = &requests[index];
    const nxandroid_owned_catalog_entry *owned;
    if (!nxandroid_valid_request(request)) {
      result = NXANDROID_EINVAL;
      goto failed;
    }
    owned = nxandroid_find_entry(catalog, request);
    if (owned == NULL) {
      if (request->criticality == NXANDROID_IMPORT_CRITICAL ||
          request->binding == NXANDROID_BIND_STRONG) {
        result = nxandroid_has_same_named_contract(catalog, request)
                     ? NXANDROID_ECONTRACT
                     : NXANDROID_EUNRESOLVED;
        goto failed;
      }
      /* The only fail-open case is explicitly non-critical and weak. */
      temporary[index].resolved = 0;
      temporary[index].address = 0u;
      continue;
    }
    if (owned->entry.provider_kind == NXANDROID_PROVIDER_STUB &&
        !request->allow_stub) {
      result = NXANDROID_ECONTRACT;
      goto failed;
    }
    temporary[index].resolved = 1;
    temporary[index].address = owned->entry.address;
    temporary[index].provider_kind = owned->entry.provider_kind;
    temporary[index].contract_id = owned->entry.contract_id;
    temporary[index].stub_semantics = owned->entry.stub_semantics;
  }

  memcpy(bindings, temporary, request_count * sizeof(*bindings));
  free(temporary);
  return NXANDROID_OK;

failed:
  if (bad_request != NULL)
    *bad_request = index;
  free(temporary);
  return result;
}
