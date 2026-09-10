/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_PROVIDER_LINUX_H
#define NXINPUT_PROVIDER_LINUX_H
/* The Linux probe behind nxinput_provider.h. See the .c for the steps. */
#include "nxinput_provider.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct nxinput_provider_probe {
  nxinput_provider_evidence evidence;
  /* FD to the object that is really mapped; kept open across the decision
   * so the bytes cannot change under it. -1 when unavailable. */
  int fd;
  /* The RetroArch-derived table API, when has_exported_bytable == 1. Post-open, per
   * instance id; -1 when SDL cannot name the ordinal. */
  int (*button_code_by_id)(int instance_id, int button);
  int (*axis_code_by_id)(int instance_id, int axis);
  int (*hat_code_by_id)(int instance_id, int hat);
  const char *(*device_path_by_id)(int instance_id);
} nxinput_provider_probe;
/* `sdl_entry` is the address of the SDL entry point this process calls
 * (for example &SDL_Init as linked, or the dlsym result the port uses). */
int nxinput_provider_probe_sdl(const void *sdl_entry, uint8_t api,
                               nxinput_provider_probe *probe);
void nxinput_provider_probe_close(nxinput_provider_probe *probe);

/* 0.11.1: MEASURED_INPROCESS. Read the provider's own ordinal table for an
 * OPENED joystick instance through the ById API the probe resolved (the
 * real calls, in the real DSO, for the real device -- never the presence of
 * the symbols). Returns 0 with `out` filled, -1 when the probe has no
 * table API or the instance is unknown to the provider (nothing measured). */
int nxinput_provider_measure_by_id(const nxinput_provider_probe *probe,
                                   int instance_id,
                                   nxinput_provider_measurement *out);

/* 0.11.1: the DEVICE probe (5.1 "joystick driver effective per device").
 * Classifies the node SDL chose and measures its capability bitmaps:
 *   evdev    a character device under /dev/input that answers EVIOCGBIT
 *   hidapi   a /dev/hidraw* node, or a character device that does not
 *            answer EVIOCGBIT (SDL's HIDAPI joystick driver: the provider
 *            synthesizes its own mapping, no ordinal table applies)
 *   virtual  an empty path (SDL virtual joystick)
 *   unknown  anything else (not a character device, unreadable)
 * `key_bits`/`abs_bits` are filled only for evdev. */
typedef struct nxinput_provider_device_probe {
  uint8_t driver;          /* nxinput_provider_driver */
  uint8_t caps_measured;   /* 1 when EVIOCGBIT answered */
  uint16_t bus;            /* EVIOCGID bustype when evdev, else 0 */
  uint16_t vendor, product, version; /* EVIOCGID when evdev (evidence only) */
  uint64_t physical_digest;/* fnv1a64 over bus/vid/pid/version/phys/uniq/caps */
} nxinput_provider_device_probe;
int nxinput_provider_probe_device(const char *devpath,
                                  unsigned long *key_bits, size_t key_words,
                                  unsigned long *abs_bits, size_t abs_words,
                                  nxinput_provider_device_probe *out);

/* 0.11.1: a RUNTIME pin file (evidence order 2 declared by a harness or a
 * CFW integration for a DSO built from a pinned source). One pin per line:
 *   <sha256-hex> <sdl2|sdl3> <domain-name> <id>
 * '#' comments and blank lines ignored; malformed lines are refused (-1,
 * nothing installed). Returns the number of pins installed via
 * nxinput_provider_set_runtime_pins(), whose storage is static here. */
#define NXINPUT_PROVIDER_PIN_FILE_MAX 16u
int nxinput_provider_load_pin_file(const char *path);
#ifdef __cplusplus
}
#endif
#endif
