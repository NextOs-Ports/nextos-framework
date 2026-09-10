/*
 * jni_shim.h -- fake JNI environment para Stardew Valley (Mono-Android).
 *
 * Reusa os offsets de vtable JNIEnv/JavaVM do Bionic arm64 LP64 (verificados
 * no port gtalcs2). Dispatch generico: metodos/campos desconhecidos recebem
 * um ID valido e caem num default seguro (0/NULL), logados na 1a ocorrencia.
 */
#ifndef __JNI_SHIM_H__
#define __JNI_SHIM_H__

#include <stddef.h>

/* Constroi fake_env + fake_vm. Devolve o ponteiro da JavaVM. */
void *jni_build_env(void);

/* Ponteiro da JNIEnv (fake_env). */
void *jni_env_ptr(void);

/* Log de chamadas JNI (default ON se SB_JNI_VERBOSE=1 ou unset->on). */
void jni_set_verbose(int on);

/* Define o diretorio de libs do app (nativeLibraryDir). Passado como unico
 * elemento do array libFolders de Runtime_init (token 0x4001) — no Android
 * real vem do Context; aqui devolvemos o path real onde estao os .so/AOT. */
void jni_set_libdir(const char *path);
/* dataDir/cacheDir do app: FORA do payload selado (ver jni_shim.c). */
void jni_set_datadir(const char *path);
void jni_set_apk_path(const char *path);


/* O ContentManager abre o XNB imediatamente antes de criar a Texture2D na
 * mesma thread. Informa ao shim GL se o ultimo Texture.xnb possui um
 * Palette.xnb companheiro: estes atlases guardam indices de paleta nos bytes
 * RGBA, portanto conversao 4444/ETC1 ou filtro por media corrompe a sprite. */
int jni_last_asset_is_paletted_texture(void);
int jni_take_pending_paletted_texture(void);
int jni_take_pending_font_texture(void);
int jni_copy_last_texture_asset_path(char *out, size_t capacity);

/* Cria handles opacos com identidade de classe/objeto. Sao usados para
 * reproduzir o Runtime.register() gerado pelo Xamarin sem uma JVM real. */
void *jni_make_class(const char *dot_name);
void *jni_make_object(void *klass);
void *jni_find_object(const char *dot_class_name);
void jni_set_key_event_keycode(void *event, int keycode, int device_id);
/* Atualiza o unico ponteiro de um MotionEvent sintetico. action usa os
 * valores Android ACTION_DOWN=0, ACTION_UP=1, ACTION_MOVE=2 e ACTION_CANCEL=3. */
void jni_set_motion_event(void *event, int action, float x, float y);
/* MotionEvent ACTION_MOVE de SOURCE_GAMEPAD|JOYSTICK, com os eixos Android
 * que o GamePad.OnGenericMotionEvent do MonoGame consome nativamente. */
void jni_set_gamepad_motion_event(void *event, int device_id,
                                  float lx, float ly,
                                  float rx, float ry, float left_trigger,
                                  float right_trigger, float hat_x,
                                  float hat_y);
void jni_set_activity(void *activity);
void *jni_activity(void);
void jni_set_main_looper_ready(int ready);

/* O fake Looper executa SyncContext.Send inline. Promove a worker atual para
 * foreground para conservar a semantica da UI thread que o Android real
 * forneceria ao MonoGame. Implementado pelo bootstrap, que possui os simbolos
 * de embedding do Mono carregado pelo so-loader. */
void sdv_promote_current_mono_thread(void);

/* Reaponta ContextManager.mainThread (Paris) para a thread atual do game
 * loop. Sem Looper real o loop nao roda na thread do onCreate como no retail
 * (RenderOnUIThread=true), e o EnsureLock() do ParisContentManager deadlocka
 * com o BlockOnUIThread do preload. Chamado a cada sdv_egl_swap ate conseguir
 * (one-shot). Implementado pelo bootstrap, como o promote acima. */
void sdv_fix_paris_mainthread(void);

/* Procura, do mais recente para o mais antigo, um handler capturado por
 * JNIEnv::RegisterNatives. `sig` pode ser NULL para casar apenas o nome. */
void *jni_find_registered_native(const char *name, const char *sig);

#endif
