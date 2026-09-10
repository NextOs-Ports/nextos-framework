/* SPDX-License-Identifier: GPL-3.0-only */
#include "otr_gptk.h"
#include "nxinput_gptk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long digital_count;
static unsigned long vector_count;
static int last_pressed;
static float last_x;
static float last_y;
static char last_action[96];

static void require(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "test_gptk_adapter: %s\n", message);
    exit(1);
  }
}

static void digital(const char *action, int pressed, float value) {
  (void)value;
  digital_count++;
  last_pressed = pressed;
  (void)snprintf(last_action, sizeof(last_action), "%s", action);
}

static void vector(const char *action, float x, float y) {
  vector_count++;
  last_x = x;
  last_y = y;
  (void)snprintf(last_action, sizeof(last_action), "%s", action);
}

static char *read_text(const char *path) {
  FILE *file = fopen(path, "rb");
  long length;
  char *text;
  require(file != NULL, "receipt missing");
  require(fseek(file, 0, SEEK_END) == 0, "receipt seek failed");
  length = ftell(file);
  require(length >= 0 && fseek(file, 0, SEEK_SET) == 0,
          "receipt length failed");
  text = (char *)calloc((size_t)length + 1u, 1u);
  require(text != NULL, "receipt allocation failed");
  require(fread(text, 1u, (size_t)length, file) == (size_t)length,
          "receipt read failed");
  require(fclose(file) == 0, "receipt close failed");
  return text;
}

int main(int argc, char **argv) {
  otr_gptk_hooks hooks = {digital, vector};
  char error[256];
  char receipt[1024];
  char load_receipt[1024];
  char *text;

  require(argc == 2, "temporary game directory argument missing");
  require(otr_gptk_init(argv[1], 640, 480, &hooks, error, sizeof(error)) == 0,
          error);
  require(strcmp(otr_gptk_context_name(), "cursor") == 0,
          "adapter did not start in the touch-menu cursor context");
  otr_gptk_set_primary_mask(UINT32_C(0x3ffff));

  otr_gptk_feed_button(NXINPUT_GPTK_A, 1, 1.0f);
  otr_gptk_feed_button(NXINPUT_GPTK_A, 1, 1.0f);
  require(digital_count == 1u && last_pressed == 1 &&
              strcmp(last_action, "otr.confirm") == 0,
          "one A press did not produce exactly one confirm delivery");
  otr_gptk_feed_button(NXINPUT_GPTK_A, 0, 0.0f);
  require(digital_count == 2u && last_pressed == 0,
          "confirm release was not delivered exactly once");

  otr_gptk_feed_stick(NXINPUT_GPTK_RIGHT_STICK, 1.0f, 0.0f, 0.016f);
  require(vector_count == 1u && strcmp(last_action, "cursor.move") == 0 &&
              last_x > 320.0f && last_y >= 0.0f,
          "right stick did not drive the canonical menu cursor");
  otr_gptk_feed_button(NXINPUT_GPTK_R3, 1, 1.0f);
  require(strcmp(last_action, "cursor.click") == 0 && last_pressed == 1,
          "R3 did not reach the click sink");
  otr_gptk_feed_button(NXINPUT_GPTK_R3, 0, 0.0f);

  otr_gptk_set_gameplay(1);
  require(strcmp(otr_gptk_context_name(), "gameplay") == 0,
          "cMulti boundary did not select gameplay");
  otr_gptk_feed_stick(NXINPUT_GPTK_LEFT_STICK, 0.5f, -0.25f, 0.016f);
  require(strcmp(last_action, "camera.steer") == 0,
          "left stick did not reach native steering");
  otr_gptk_feed_stick(NXINPUT_GPTK_RIGHT_STICK, -0.25f, 0.5f, 0.016f);
  require(strcmp(last_action, "camera.look") == 0,
          "right stick was not returned to the native camera in gameplay");

  (void)snprintf(receipt, sizeof(receipt), "%s/input.json", argv[1]);
  (void)snprintf(load_receipt, sizeof(load_receipt), "%s/load.json", argv[1]);
  require(otr_gptk_write_receipt(receipt) == 0,
          "input receipt was not written");
  require(otr_gptk_write_load_receipt(load_receipt) == 0,
          "load receipt was not written");
  text = read_text(receipt);
  require(strstr(text, "\"delivery_count\":1") != NULL &&
              strstr(text, "\"double_input\":false") != NULL,
          "receipt did not prove exactly-one semantic delivery");
  require(strstr(text, "\"duplicate_deliveries\":0") != NULL,
          "receipt reported a duplicate semantic delivery");
  require(strstr(text, "\"deduplicated_edges\":1") != NULL,
          "receipt did not record the repeated physical edge suppression");
  free(text);
  text = read_text(load_receipt);
  require(strstr(text, "\"source\":\"default_owner_missing\"") != NULL,
          "immutable default load was not receipted");
  free(text);

  /* The shell installs a valid owner mapping with A/B swapped before this
   * second initialization.  Runtime selection must prefer it without changing
   * the adapter or compiling a second mapping path. */
  {
    char owner[1024], staged[1032];
    FILE *src;
    FILE *dst;
    unsigned char buffer[4096];
    size_t count;
    const char *fixture = getenv("OTR_SWAPPED_GPTK");
    require(fixture != NULL, "swapped GPTK fixture path missing");
    (void)snprintf(owner, sizeof(owner), "%s/NEXTOSCONTROLLERS.gptk", argv[1]);
    (void)snprintf(staged, sizeof(staged), "%s.tmp", owner);
    src = fopen(fixture, "rb");
    dst = fopen(staged, "wb");
    require(src != NULL && dst != NULL, "cannot stage swapped owner mapping");
    while ((count = fread(buffer, 1u, sizeof(buffer), src)) > 0u)
      require(fwrite(buffer, 1u, count, dst) == count,
              "cannot copy swapped owner mapping");
    require(ferror(src) == 0 && fclose(src) == 0 && fclose(dst) == 0 &&
                rename(staged, owner) == 0,
            "cannot publish swapped owner mapping");
  }
  digital_count = 0u;
  vector_count = 0u;
  require(otr_gptk_init(argv[1], 640, 480, &hooks, error, sizeof(error)) == 0,
          error);
  otr_gptk_set_primary_mask(UINT32_C(0x3ffff));
  otr_gptk_feed_button(NXINPUT_GPTK_A, 1, 1.0f);
  require(digital_count == 1u && strcmp(last_action, "otr.back") == 0,
          "editable owner mapping did not swap A/B");
  otr_gptk_feed_button(NXINPUT_GPTK_A, 0, 0.0f);
  otr_gptk_feed_button(NXINPUT_GPTK_B, 1, 1.0f);
  require(strcmp(last_action, "otr.confirm") == 0,
          "editable owner mapping did not swap B/A");
  otr_gptk_feed_button(NXINPUT_GPTK_B, 0, 0.0f);
  otr_gptk_note_terminal_receipt("sdl-primary", 1u, 42u, 1);
  require(otr_gptk_write_receipt(receipt) == 0,
          "swapped input receipt was not written");
  text = read_text(receipt);
  require(strstr(text, "\"ab_swap_configured\":true") != NULL &&
              strstr(text, "\"ab_swap_observed\":true") != NULL,
          "receipt did not prove the edited A/B mapping was exercised");
  require(strstr(text, "\"start_select_observed\":true") != NULL &&
              strstr(text, "\"terminal_source\":\"sdl-primary\"") != NULL &&
              strstr(text, "\"terminal_delivery_count\":1") != NULL &&
              strstr(text, "\"terminal_polls\":42") != NULL &&
              strstr(text, "\"terminal_independent_guest_loop\":true") != NULL &&
              strstr(text, "\"delivery_count\":1") != NULL &&
              strstr(text, "\"double_input\":false") != NULL,
          "receipt did not prove chord/exactly-one delivery");
  free(text);
  require(otr_gptk_write_load_receipt(load_receipt) == 0,
          "owner load receipt was not written");
  text = read_text(load_receipt);
  require(strstr(text, "\"source\":\"owner\"") != NULL,
          "valid editable owner mapping was not selected");
  free(text);
  puts("test_gptk_adapter: PASS");
  return 0;
}
