/* Completa o pad pelo evdev, sem depender do mapping da SDL.
 *
 * Motivo (medido no R36S/ArkOS, receita ja' aprovada em ports/chrono):
 * a SDL calcula para o pad um GUID com campo de CRC que NAO casa com a linha
 * do gamecontrollerdb; sem casar, ela monta um mapping automatico que declara
 * so' `a0-a3` e `b0-b11` -- fica sem `back`, `start`, `leftstick` e
 * `rightstick`. Nestes portateis SELECT/START/L3/R3 chegam como
 * BTN_TRIGGER_HAPPY1..4, que e' exatamente o que o mapping automatico ignora.
 *
 * Ler o evdev resolve para QUALQUER aparelho e sem cravar indice de botao: os
 * codigos BTN_* sao ABI estavel do kernel. Quem tem o pad completo pela SDL
 * nao muda de comportamento -- os dois caminhos so' se somam (OR).
 */
#define _GNU_SOURCE

#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "pad_evdev.h"
#include "util.h"

/* BTN_* nos headers novos, KEY_* nos antigos; os codigos sao ABI estavel. */
#define PAD_KEY_TRIGGER_HAPPY1 0x2c0 /* SELECT nestes portateis */
#define PAD_KEY_TRIGGER_HAPPY2 0x2c1 /* START  */
#define PAD_KEY_TRIGGER_HAPPY3 0x2c2 /* L3     */
#define PAD_KEY_TRIGGER_HAPPY4 0x2c3 /* R3     */

#define PAD_EVDEV_DEVICES 8

static int g_fd[PAD_EVDEV_DEVICES];
static int g_count = -1;
static unsigned char g_select, g_start, g_l3, g_r3;

#define PAD_BIT_SET(bits, code)                                             \
  ((bits[(code) / (8 * sizeof(unsigned long))] >>                           \
    ((code) % (8 * sizeof(unsigned long)))) & 1UL)

static int is_gamepad(int fd) {
  /* O tamanho do bitmap e' o do LEITOR (unsigned long do nosso processo). */
  unsigned long keys[(KEY_MAX / (8 * sizeof(unsigned long))) + 1];
  memset(keys, 0, sizeof keys);
  if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0)
    return 0;
  return PAD_BIT_SET(keys, BTN_SOUTH) || PAD_BIT_SET(keys, BTN_A) ||
         PAD_BIT_SET(keys, PAD_KEY_TRIGGER_HAPPY1);
}

void pad_evdev_rescan(void) {
  /* Controle plugado DEPOIS da abertura tem um /dev/input/event novo, que a
   * varredura inicial nao viu -- sem reabrir, SELECT/START/L3/R3 desse pad
   * ficariam de fora justamente nos aparelhos onde eles so' existem no evdev.
   * Chamado no hotplug da SDL, que e' quando a lista muda. */
  pad_evdev_close();
  pad_evdev_open();
}

void pad_evdev_open(void) {
  if (g_count >= 0)
    return;
  g_count = 0;
  for (int index = 0; index < 32 && g_count < PAD_EVDEV_DEVICES; ++index) {
    char path[64];
    snprintf(path, sizeof path, "/dev/input/event%d", index);
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
      continue;
    if (!is_gamepad(fd)) {
      close(fd);
      continue;
    }
    char name[128] = "?";
    (void)ioctl(fd, EVIOCGNAME(sizeof name), name);
    debugPrintf("[pad] evdev %s (%s) -- SELECT/START/L3/R3 tambem por aqui\n",
                path, name);
    g_fd[g_count++] = fd;
  }
  if (!g_count)
    debugPrintf("[pad] nenhum evdev de controle legivel; SELECT/START/R3 "
                "dependem so' do mapping da SDL\n");
}

void pad_evdev_poll(void) {
  if (g_count <= 0)
    return;
  struct input_event event;
  for (int i = 0; i < g_count; ++i) {
    while (read(g_fd[i], &event, sizeof event) == (ssize_t)sizeof event) {
      if (event.type != EV_KEY)
        continue;
      unsigned char down = event.value != 0; /* 1 press, 2 autorepeat */
      switch (event.code) {
        case BTN_SELECT:
        case PAD_KEY_TRIGGER_HAPPY1: g_select = down; break;
        case BTN_START:
        case PAD_KEY_TRIGGER_HAPPY2: g_start = down; break;
        case BTN_THUMBL:
        case PAD_KEY_TRIGGER_HAPPY3: g_l3 = down; break;
        case BTN_THUMBR:
        case PAD_KEY_TRIGGER_HAPPY4: g_r3 = down; break;
        default: break;
      }
    }
  }
}

int pad_evdev_select(void) { return g_select; }
int pad_evdev_start(void) { return g_start; }
int pad_evdev_l3(void) { return g_l3; }
int pad_evdev_r3(void) { return g_r3; }

void pad_evdev_close(void) {
  for (int i = 0; i < g_count && i < PAD_EVDEV_DEVICES; ++i)
    close(g_fd[i]);
  g_count = -1;
  g_select = g_start = g_l3 = g_r3 = 0;
}
