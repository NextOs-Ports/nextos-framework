#ifndef CT_PLATFORM_H
#define CT_PLATFORM_H

struct SDL_Window;

const char *ct_gamedir(void);

void ct_request_exit(const char *reason);
int ct_exit_requested(void);
const char *ct_exit_reason(void);
void ct_install_signal_handlers(void);
void ct_start_shutdown_watchdog(int seconds);

int ct_single_instance_lock(const char *self_path);
void ct_single_instance_unlock(void);

struct _SDL_GameController;
void ct_exit_chord_set_controller(struct _SDL_GameController *pad);
void ct_exit_chord_open(void);
void ct_exit_chord_poll(void);
void ct_exit_chord_close(void);

/* Abre o SDL_INIT_AUDIO em separado do video: falha de audio nunca e' fatal. */
void ct_init_audio_subsystem(void);
void ct_log_video_profile(struct SDL_Window *window, int drawable_w, int drawable_h);
int ct_needs_finish_before_swap(void);

long ct_peak_rss_kb(void);
long ct_mem_total_kb(void);

#endif /* CT_PLATFORM_H */
