#ifndef PAD_EVDEV_H
#define PAD_EVDEV_H

/* Estado cru de SELECT/START/L3/R3 lido do evdev, para somar (OR) ao que a SDL
 * entrega. Ver pad_evdev.c: em varios portateis esses quatro botoes chegam
 * como BTN_TRIGGER_HAPPY1..4 e o mapping automatico da SDL nao os declara. */
void pad_evdev_open(void);
void pad_evdev_poll(void);
void pad_evdev_rescan(void);
void pad_evdev_close(void);
int pad_evdev_select(void);
int pad_evdev_start(void);
int pad_evdev_l3(void);
int pad_evdev_r3(void);

#endif /* PAD_EVDEV_H */
