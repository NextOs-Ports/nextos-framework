#include "shop_compat.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jni_bridge.h"
#include "so_util.h"
#include "util.h"

enum {
#if defined(__i386__)
  ASM2_STORE_MANAGER_SLOT_VMA = 0x01eabb50u,
  ASM2_IAB_STORE_SLOT_VMA = 0x01ed5e7cu,
  ASM2_STORE_PARSE_VMA = 0x004b5680u,
  ASM2_STORE_CATEGORY_COUNT_VMA = 0x004bcec0u,
  ASM2_GUEST_STRING_CONSTRUCTOR_VMA = 0x01993650u,
  ASM2_STORE_MANAGER_SLOT_REFERENCE_VMA = 0x004bb451u,
  ASM2_IAB_STORE_SLOT_REFERENCE_VMA = 0x01008e62u,
#else
  ASM2_STORE_MANAGER_SLOT_VMA = 0x0134f728u,
  ASM2_IAB_STORE_SLOT_VMA = 0x01378508u,
  ASM2_STORE_PARSE_VMA = 0x003986d4u,
  ASM2_STORE_CATEGORY_COUNT_VMA = 0x0037d488u,
  ASM2_GUEST_STRING_CONSTRUCTOR_VMA = 0x01128de8u,
#endif

  ASM2_STORE_MANAGER_DESCRIPTOR_COUNT_OFFSET = 0x40u,
  ASM2_STORE_MANAGER_REQUEST_OFFSET = 0x44u,
  ASM2_STORE_MANAGER_STATE_OFFSET = 0x48u,
  ASM2_IAB_STORE_ITEM_COUNT_OFFSET = 0x54u,

  ASM2_EXPECTED_DESCRIPTOR_COUNT = 50u,
  ASM2_EXPECTED_IAB_ITEM_COUNT = 18u,
  ASM2_CATALOG_SIZE = 9577u,
  ASM2_STORE_ITEMS_SIZE = 4717u,
  ASM2_CATALOG_CRC32 = 0x036b8ff1u,
  ASM2_STORE_ITEMS_CRC32 = 0x1d64c20au,

  ASM2_STORE_STATE_PARSED = 5,
};

static const char catalog_relative_path[] =
    "assets/IapLocalData_Google.json";
static const char store_items_relative_path[] =
    "assets/IapStoreItems_Offline.json";

typedef void *(ASM2_GUEST_PCS *asm2_billing_send_data_fn)(
    void *environment, void *class_handle, void *bundle);
typedef void *(ASM2_GUEST_PCS *asm2_guest_string_constructor_fn)(
    void *destination, const char *source, const void *allocator);
typedef void(ASM2_GUEST_PCS *asm2_guest_store_parse_fn)(
    void *manager, const void *json_string);
typedef unsigned(ASM2_GUEST_PCS *asm2_guest_store_category_count_fn)(
    void *manager, unsigned category);

static uint32_t *store_manager_slot;
static asm2_guest_string_constructor_fn guest_string_constructor;
static asm2_guest_store_parse_fn guest_store_parse;
static asm2_guest_store_category_count_fn guest_store_category_count;
static char *catalog_json;
static void *guest_catalog_string;
static unsigned manager_ready_frames;
static int shop_state;
static int last_manager_state = INT32_MIN;

#if defined(__i386__)
static const unsigned char store_parse_signature[] = {
    0x8d, 0x4c, 0x24, 0x04, 0x83, 0xe4, 0xf0, 0xff,
    0x71, 0xfc, 0x55, 0x89, 0xe5, 0x57, 0x56, 0x8d,
};
static const unsigned char store_category_count_signature[] = {
    0x56, 0x53, 0xe8, 0x9c, 0xd3, 0xc2, 0xff, 0x81,
    0xc3, 0x15, 0x6b, 0x9d, 0x01, 0x8d, 0x64, 0x24,
};
static const unsigned char guest_string_constructor_signature[] = {
    0x56, 0xb8, 0xff, 0xff, 0xff, 0xff, 0x53, 0xe8,
    0x07, 0x6c, 0x75, 0xfe, 0x81, 0xc3, 0x80, 0x03,
};
static const unsigned char store_manager_slot_reference_signature[] = {
    0x89, 0xb3, 0x74, 0x81, 0x01, 0x00,
};
static const unsigned char iab_store_slot_reference_signature[] = {
    0x8b, 0x83, 0xa0, 0x24, 0x04, 0x00,
};
#endif

static uint32_t crc32_bytes(const unsigned char *data, size_t size) {
  uint32_t crc = UINT32_MAX;
  for (size_t index = 0; index < size; ++index) {
    crc ^= data[index];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^
            (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
  }
  return ~crc;
}

static char *read_versioned_asset(const char *game_directory,
                                  const char *relative_path,
                                  size_t expected_size,
                                  uint32_t expected_crc32) {
  if (!game_directory || !relative_path) {
    errno = EINVAL;
    return NULL;
  }

  size_t directory_size = strlen(game_directory);
  size_t relative_size = strlen(relative_path);
  if (directory_size > SIZE_MAX - relative_size - 2u) {
    errno = ENAMETOOLONG;
    return NULL;
  }

  char *path = malloc(directory_size + relative_size + 2u);
  if (!path)
    return NULL;
  memcpy(path, game_directory, directory_size);
  size_t offset = directory_size;
  if (!offset || path[offset - 1] != '/')
    path[offset++] = '/';
  memcpy(path + offset, relative_path, relative_size + 1u);

  FILE *input = fopen(path, "rb");
  if (!input) {
    debugPrintf("ASM2_SHOP_ASSET_ERROR open=%s error=%s\n", path,
                strerror(errno));
    free(path);
    return NULL;
  }

  char *contents = malloc(expected_size + 1u);
  if (!contents) {
    int saved_errno = errno;
    fclose(input);
    free(path);
    errno = saved_errno;
    return NULL;
  }
  size_t received = fread(contents, 1, expected_size + 1u, input);
  int read_error = ferror(input);
  fclose(input);

  if (read_error || received != expected_size ||
      crc32_bytes((const unsigned char *)contents, received) !=
          expected_crc32) {
    debugPrintf("ASM2_SHOP_ASSET_ERROR invalid=%s bytes=%zu expected=%zu\n",
                path, received, expected_size);
    free(contents);
    free(path);
    errno = EINVAL;
    return NULL;
  }

  contents[received] = '\0';
  free(path);
  return contents;
}

static int resolve_versioned_guest_api(void) {
  store_manager_slot =
      so_guest_vma(ASM2_STORE_MANAGER_SLOT_VMA, sizeof(*store_manager_slot));
  uint32_t *iab_store_slot =
      so_guest_vma(ASM2_IAB_STORE_SLOT_VMA, sizeof(*iab_store_slot));
#if defined(__i386__)
  unsigned char *parse_instruction = so_guest_vma(
      ASM2_STORE_PARSE_VMA, sizeof(store_parse_signature));
  unsigned char *count_instruction = so_guest_vma(
      ASM2_STORE_CATEGORY_COUNT_VMA, sizeof(store_category_count_signature));
  unsigned char *string_instruction =
      so_guest_vma(ASM2_GUEST_STRING_CONSTRUCTOR_VMA,
                   sizeof(guest_string_constructor_signature));
  unsigned char *manager_slot_reference =
      so_guest_vma(ASM2_STORE_MANAGER_SLOT_REFERENCE_VMA,
                   sizeof(store_manager_slot_reference_signature));
  unsigned char *iab_slot_reference =
      so_guest_vma(ASM2_IAB_STORE_SLOT_REFERENCE_VMA,
                   sizeof(iab_store_slot_reference_signature));

  if (!store_manager_slot || !iab_store_slot || !parse_instruction ||
      !count_instruction || !string_instruction ||
      !manager_slot_reference || !iab_slot_reference ||
      memcmp(parse_instruction, store_parse_signature,
             sizeof(store_parse_signature)) != 0 ||
      memcmp(count_instruction, store_category_count_signature,
             sizeof(store_category_count_signature)) != 0 ||
      memcmp(string_instruction, guest_string_constructor_signature,
             sizeof(guest_string_constructor_signature)) != 0 ||
      memcmp(manager_slot_reference, store_manager_slot_reference_signature,
             sizeof(store_manager_slot_reference_signature)) != 0 ||
      memcmp(iab_slot_reference, iab_store_slot_reference_signature,
             sizeof(iab_store_slot_reference_signature)) != 0) {
    debugPrintf("ASM2_SHOP_VERSION_ERROR x86 manager=%p iab=%p "
                "parse=%p count=%p string=%p manager_ref=%p iab_ref=%p\n",
                (void *)store_manager_slot, (void *)iab_store_slot,
                (void *)parse_instruction, (void *)count_instruction,
                (void *)string_instruction, (void *)manager_slot_reference,
                (void *)iab_slot_reference);
    errno = EINVAL;
    return -1;
  }
#else
  uint32_t *parse_instruction =
      so_guest_vma(ASM2_STORE_PARSE_VMA, sizeof(*parse_instruction));
  uint32_t *count_instruction =
      so_guest_vma(ASM2_STORE_CATEGORY_COUNT_VMA, sizeof(*count_instruction));
  uint32_t *string_instruction = so_guest_vma(
      ASM2_GUEST_STRING_CONSTRUCTOR_VMA, sizeof(*string_instruction));

  if (!store_manager_slot || !iab_store_slot || !parse_instruction ||
      !count_instruction || !string_instruction ||
      *parse_instruction != 0xe59fcfa0u ||
      *count_instruction != 0xe59f308cu ||
      *string_instruction != 0xe92d4070u) {
    debugPrintf("ASM2_SHOP_VERSION_ERROR manager=%p iab=%p "
                "parse=%08x count=%08x string=%08x\n",
                (void *)store_manager_slot, (void *)iab_store_slot,
                parse_instruction ? (unsigned)*parse_instruction : 0u,
                count_instruction ? (unsigned)*count_instruction : 0u,
                string_instruction ? (unsigned)*string_instruction : 0u);
    errno = EINVAL;
    return -1;
  }
#endif

  guest_store_parse = (asm2_guest_store_parse_fn)parse_instruction;
  guest_store_category_count =
      (asm2_guest_store_category_count_fn)count_instruction;
  guest_string_constructor =
      (asm2_guest_string_constructor_fn)string_instruction;
  return 0;
}

int asm2_shop_compat_initialize(const char *game_directory,
                                uintptr_t billing_native_send_data,
                                void *billing_class) {
  shop_state = -1;
  manager_ready_frames = 0;
  last_manager_state = INT32_MIN;

  if (!billing_native_send_data || !billing_class ||
      resolve_versioned_guest_api() != 0)
    return -1;

  catalog_json = read_versioned_asset(
      game_directory, catalog_relative_path, ASM2_CATALOG_SIZE,
      ASM2_CATALOG_CRC32);
  char *store_items_json = read_versioned_asset(
      game_directory, store_items_relative_path, ASM2_STORE_ITEMS_SIZE,
      ASM2_STORE_ITEMS_CRC32);
  if (!catalog_json || !store_items_json) {
    free(catalog_json);
    catalog_json = NULL;
    free(store_items_json);
    return -1;
  }

  void *request = asm2_jni_bundle();
  if (!request || !asm2_jni_bundle_put_int(request, "O", 17) ||
      !asm2_jni_bundle_put_byte_array(request, "R", store_items_json,
                                     ASM2_STORE_ITEMS_SIZE)) {
    debugPrintf("ASM2_SHOP_IAB_ERROR bundle allocation\n");
    free(store_items_json);
    return -1;
  }

  debugPrintf("ASM2_SHOP_IAB_BEGIN operation=17 entries=%u\n",
              ASM2_EXPECTED_IAB_ITEM_COUNT);
  ((asm2_billing_send_data_fn)billing_native_send_data)(
      asm2_jni_env(), billing_class, request);
  free(store_items_json);

  uint32_t *iab_store_slot =
      so_guest_vma(ASM2_IAB_STORE_SLOT_VMA, sizeof(*iab_store_slot));
  uintptr_t iab_store = iab_store_slot ? (uintptr_t)*iab_store_slot : 0u;
  unsigned item_count =
      iab_store
          ? *(const uint32_t *)(iab_store + ASM2_IAB_STORE_ITEM_COUNT_OFFSET)
          : 0u;
  if (item_count != ASM2_EXPECTED_IAB_ITEM_COUNT) {
    debugPrintf("ASM2_SHOP_IAB_ERROR parsed=%u expected=%u object=%p\n",
                item_count, ASM2_EXPECTED_IAB_ITEM_COUNT,
                (void *)iab_store);
    errno = EINVAL;
    return -1;
  }

  shop_state = 0;
  debugPrintf("ASM2_SHOP_IAB_OK parsed=%u\n", item_count);
  return 0;
}

void asm2_shop_compat_tick(void) {
  if (shop_state != 0 || !store_manager_slot || !catalog_json)
    return;

  uintptr_t manager_address = (uintptr_t)*store_manager_slot;
  if (!manager_address) {
    manager_ready_frames = 0;
    return;
  }

  unsigned char *manager = (unsigned char *)manager_address;
  unsigned descriptor_count = *(const uint32_t *)(
      manager + ASM2_STORE_MANAGER_DESCRIPTOR_COUNT_OFFSET);
  uintptr_t request = *(const uint32_t *)(
      manager + ASM2_STORE_MANAGER_REQUEST_OFFSET);
  int manager_state = *(const int32_t *)(
      manager + ASM2_STORE_MANAGER_STATE_OFFSET);

  if (manager_state != last_manager_state) {
    debugPrintf("ASM2_SHOP_MANAGER_STATE state=%d descriptors=%u request=%p\n",
                manager_state, descriptor_count, (void *)request);
    last_manager_state = manager_state;
    manager_ready_frames = 0;
  }

  if (descriptor_count != ASM2_EXPECTED_DESCRIPTOR_COUNT || !request) {
    manager_ready_frames = 0;
    return;
  }
  ++manager_ready_frames;

  /* Let the unmodified state machine consume its own local fallback first.
   * The native IAB map was populated before the first game frame, so that
   * path normally succeeds without any manager intervention. */
  if (manager_state < 3 && manager_state >= 0)
    return;

  unsigned suit_count = guest_store_category_count(manager, 1u);
  unsigned item_count = guest_store_category_count(manager, 2u);
  if (suit_count || item_count) {
    shop_state = 1;
    debugPrintf("ASM2_SHOP_CATALOG_OK source=native suits=%u items=%u "
                "state=%d\n",
                suit_count, item_count, manager_state);
    return;
  }

  /* -3 is the game's explicit terminal state for an empty Suit category.
   * State 3/4/5 can also remain idle when the retired config-storage service
   * supplies no body; give those states three seconds before using the exact
   * catalog shipped in the 1.2.7d OBB. */
  if (manager_state != -3 && manager_ready_frames < 180u)
    return;

  unsigned char allocator = 0;
  guest_string_constructor(&guest_catalog_string, catalog_json, &allocator);
  if (!guest_catalog_string) {
    debugPrintf("ASM2_SHOP_CATALOG_ERROR guest string allocation\n");
    shop_state = -1;
    return;
  }

  debugPrintf("ASM2_SHOP_CATALOG_BEGIN bytes=%u state=%d\n",
              ASM2_CATALOG_SIZE, manager_state);
  guest_store_parse(manager, &guest_catalog_string);

  suit_count = guest_store_category_count(manager, 1u);
  item_count = guest_store_category_count(manager, 2u);
  if (!suit_count && !item_count) {
    debugPrintf("ASM2_SHOP_CATALOG_ERROR suits=%u items=%u\n", suit_count,
                item_count);
    shop_state = -1;
    return;
  }

  *(int32_t *)(manager + ASM2_STORE_MANAGER_STATE_OFFSET) =
      ASM2_STORE_STATE_PARSED;
  shop_state = 1;
  debugPrintf("ASM2_SHOP_CATALOG_OK suits=%u items=%u state=%d\n",
              suit_count, item_count, ASM2_STORE_STATE_PARSED);
}
