#ifndef OCEAN_INPUT_H
#define OCEAN_INPUT_H

/*
 * Ponte de controle do Oceanhorn: SDL_GameController -> KeyEvent/MotionEvent
 * Android -> UnityPlayer.nativeInjectEvent, com o deviceId do InputDevice
 * virtual publicado pelo jni_shim. É o caminho que o Rewired_Android lê.
 */
void ocean_input_init(void);
void ocean_input_poll(void *env, void *thiz, void *inject);
int ocean_input_exit_requested(void);
void ocean_input_notify_event(const void *sdl_event);
void ocean_input_shutdown(void);

#endif
