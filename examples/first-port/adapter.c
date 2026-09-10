/* SPDX-License-Identifier: GPL-3.0-only */
/* NextOS: AArch64 nxloader integration, narrow JNI, system SDL2/GLES2. */
#define _POSIX_C_SOURCE 200809L
#include "demo.h"
#include "nxloader.h"
#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pthread_t owner;
static DemoEnv environment;
static int32_t get_version(DemoEnv *env) {
    return env==&environment && pthread_equal(owner,pthread_self()) ? DEMO_JNI_VERSION : 0;
}
static int32_t get_env(DemoVM *vm, void **out, int32_t version) {
    (void)vm;
    if(!out) return -1;
    *out=NULL;
    if(version!=DEMO_JNI_VERSION) return -3;
    if(!pthread_equal(owner,pthread_self())) return -2;
    *out=&environment; return 0;
}
static int32_t unsupported_vm(DemoVM *vm) { (void)vm; return -1; }
static int32_t unsupported_attach(DemoVM *vm,void **env,void *args) {
    (void)vm; (void)args; if(env) *env=NULL; return -1;
}
static const struct demo_jni_table jni={ {NULL,NULL,NULL,NULL}, get_version };
static const struct demo_vm_table invocation={ {NULL,NULL,NULL},unsupported_vm,
    unsupported_attach,unsupported_vm,get_env,unsupported_attach };
static DemoVM vm=&invocation;
static int *android_errno(void) { return &errno; }
static int android_log(int priority,const char *tag,const char *message) {
    if(!tag || !message) return -EINVAL;
    return fprintf(stderr,"guest[%d] %s: %s\n",priority,tag,message)<0 ? -EIO : 1;
}
static void loader_log(void *data,nxloader_log_level level,const char *message) {
    (void)data; fprintf(stderr,"loader[%d] %s\n",(int)level,message);
}
static void check(nxloader_result result,const char *operation) {
    if(result!=NXLOADER_OK) {
        fprintf(stderr,"FAIL %s: %s\n",operation,nxloader_result_string(result)); exit(2);
    }
}
static struct {
    int (*create)(int); int (*resume)(void); int (*step)(unsigned,unsigned);
    int (*render)(uint32_t *,unsigned); int (*audio)(int16_t *,unsigned);
    int (*pause)(void); int (*save)(void); int (*restore)(int); int (*destroy)(void);
} game;
#define LOAD(field,name) do { uintptr_t address=0; \
    check(nxloader_module_find_export(module,name,&address),name); \
    _Static_assert(sizeof(game.field)==sizeof(address),"AArch64 function pointer"); \
    memcpy(&game.field,&address,sizeof(address)); } while(0)
#define EXPECT(expr) do { if(!(expr)) {fprintf(stderr,"FAIL contract: %s\n",#expr); return 3;} } while(0)

static int self_test(void) {
    uint32_t pixels[DEMO_WIDTH*DEMO_HEIGHT];
    int16_t pcm[800];
    unsigned i, changed=0, audible=0;
    EXPECT(game.create(7)==0);
    EXPECT(game.step(0,16)==-1);
    EXPECT(game.restore(3)==0);
    EXPECT(game.resume()==0);
    EXPECT(game.resume()==-1);
    EXPECT(game.step(0,251)==-1);
    for(i=0;i<100;i++) EXPECT(game.step(DEMO_RIGHT,16)==0);
    EXPECT(game.render(pixels,DEMO_WIDTH*DEMO_HEIGHT)==0);
    EXPECT(game.render(pixels,1)==-1);
    EXPECT(game.audio(pcm,800)==0);
    for(i=1;i<DEMO_WIDTH*DEMO_HEIGHT;i++) changed+=pixels[i]!=pixels[0];
    for(i=0;i<800;i++) audible+=pcm[i]!=0;
    EXPECT(changed>25 && audible>0);
    EXPECT(game.save()==-1);
    EXPECT(game.pause()==0);
    EXPECT(game.save()==4);
    EXPECT(game.destroy()==0);
    EXPECT(game.resume()==-1);
    EXPECT(game.create(7)==0);
    EXPECT(game.restore(4)==0);
    EXPECT(game.save()==4);
    EXPECT(game.destroy()==0);
    puts("PASS: Android guest executed; constructors/JNI/lifecycle/input/pixels/PCM/save contracts");
    puts("Scope: CPU contract test; no physical GPU, audio, controller or release claim");
    return 0;
}
static GLuint shader(GLenum type,const char *source) {
    GLint ok=0; GLuint object=glCreateShader(type); char message[1024];
    glShaderSource(object,1,&source,NULL); glCompileShader(object);
    glGetShaderiv(object,GL_COMPILE_STATUS,&ok);
    if(!ok) {glGetShaderInfoLog(object,sizeof(message),NULL,message);fprintf(stderr,"shader: %s\n",message);glDeleteShader(object);return 0;}
    return object;
}
static int interactive(void) {
    SDL_Window *window=NULL; SDL_GLContext context=NULL; SDL_GameController *pad=NULL;
    SDL_AudioDeviceID audio=0; SDL_AudioSpec spec={0};
    GLuint vertex=0,fragment=0,program=0,texture=0; GLint linked=0;
    uint32_t pixels[DEMO_WIDTH*DEMO_HEIGHT]; int16_t pcm[800];
    const GLfloat quad[]={-1,-1, 1,-1, -1,1, 1,1};
    int result=4,running=1,active=0,created=0;
    unsigned previous=0;
    if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO|SDL_INIT_GAMECONTROLLER)!=0) goto end;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,0);
    window=SDL_CreateWindow("NextOS training / Treino - arrows/dpad; Esc/Back to save and exit",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,640,480,SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE);
    if(!window || !(context=SDL_GL_CreateContext(window))) goto end;
    fprintf(stderr,"GL_VERSION=%s renderer=%s\n",glGetString(GL_VERSION),glGetString(GL_RENDERER));
    vertex=shader(GL_VERTEX_SHADER,"attribute vec2 p;varying vec2 uv;void main(){uv=vec2((p.x+1.0)*0.5,(1.0-p.y)*0.5);gl_Position=vec4(p,0.0,1.0);}");
    fragment=shader(GL_FRAGMENT_SHADER,"precision mediump float;varying vec2 uv;uniform sampler2D image;void main(){gl_FragColor=texture2D(image,uv);}");
    if(!vertex || !fragment) goto end;
    program=glCreateProgram(); glAttachShader(program,vertex); glAttachShader(program,fragment);
    glBindAttribLocation(program,0,"p"); glLinkProgram(program); glGetProgramiv(program,GL_LINK_STATUS,&linked);
    if(!linked) goto end;
    glUseProgram(program); glUniform1i(glGetUniformLocation(program,"image"),0);
    glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    spec.freq=48000;spec.format=AUDIO_S16SYS;spec.channels=1;spec.samples=1024;
    audio=SDL_OpenAudioDevice(NULL,0,&spec,NULL,0); if(!audio) goto end;
    SDL_PauseAudioDevice(audio,0);
    if(game.create(7)!=0) goto end;
    created=1;
    { FILE *save=fopen("score.txt","r"); int value;
      if(save) {if(fscanf(save,"%d",&value)!=1 || game.restore(value)!=0) {fclose(save);goto end;} fclose(save);} }
    if(game.resume()!=0) goto end;
    active=1; previous=SDL_GetTicks();
    while(running) {
        SDL_Event event; unsigned buttons=0,now,elapsed;const Uint8 *keys;
        while(SDL_PollEvent(&event)) {
            if(event.type==SDL_QUIT || (event.type==SDL_KEYDOWN && event.key.keysym.sym==SDLK_ESCAPE)) running=0;
            if(event.type==SDL_WINDOWEVENT && event.window.event==SDL_WINDOWEVENT_FOCUS_LOST && active) {
                if(game.pause()!=0) goto end;
                active=0;SDL_ClearQueuedAudio(audio);
            }
            if(event.type==SDL_WINDOWEVENT && event.window.event==SDL_WINDOWEVENT_FOCUS_GAINED && !active) {
                if(game.resume()!=0) goto end;
                active=1;previous=SDL_GetTicks();
            }
            if(event.type==SDL_CONTROLLERDEVICEADDED && !pad && SDL_IsGameController(event.cdevice.which)) pad=SDL_GameControllerOpen(event.cdevice.which);
            if(event.type==SDL_CONTROLLERDEVICEREMOVED && pad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad))==event.cdevice.which) {SDL_GameControllerClose(pad);pad=NULL;}
        }
        if(!running) break;
        if(!active) {SDL_Delay(16);continue;}
        keys=SDL_GetKeyboardState(NULL);
        if(keys[SDL_SCANCODE_LEFT]) buttons|=DEMO_LEFT;
        if(keys[SDL_SCANCODE_RIGHT]) buttons|=DEMO_RIGHT;
        if(keys[SDL_SCANCODE_UP]) buttons|=DEMO_UP;
        if(keys[SDL_SCANCODE_DOWN]) buttons|=DEMO_DOWN;
        if(pad) {
            Sint16 axis_x=SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_LEFTX);
            Sint16 axis_y=SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_LEFTY);
            if(axis_x < -8000) buttons|=DEMO_LEFT;
            if(axis_x > 8000) buttons|=DEMO_RIGHT;
            if(axis_y < -8000) buttons|=DEMO_UP;
            if(axis_y > 8000) buttons|=DEMO_DOWN;
            if(SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_DPAD_LEFT)) buttons|=DEMO_LEFT;
            if(SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) buttons|=DEMO_RIGHT;
            if(SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_DPAD_UP)) buttons|=DEMO_UP;
            if(SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_DPAD_DOWN)) buttons|=DEMO_DOWN;
            if(SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_BACK) || SDL_GameControllerGetButton(pad,SDL_CONTROLLER_BUTTON_START)) running=0;
        }
        now=SDL_GetTicks();elapsed=now-previous;previous=now;if(elapsed>250) elapsed=250;
        if(game.step(buttons,elapsed)!=0 || game.render(pixels,DEMO_WIDTH*DEMO_HEIGHT)!=0) goto end;
        {int width,height;SDL_GL_GetDrawableSize(window,&width,&height);glViewport(0,0,width,height);}
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,DEMO_WIDTH,DEMO_HEIGHT,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
        glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,quad);glEnableVertexAttribArray(0);glDrawArrays(GL_TRIANGLE_STRIP,0,4);
        {unsigned char probe[4]={0};int width,height;
         SDL_GL_GetDrawableSize(window,&width,&height);
         if(width<1 || height<1) goto end;
         glReadPixels(width/2,height/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,probe);
         if(!(probe[0]|probe[1]|probe[2])) {fputs("FAIL: black center pixel before present\n",stderr);goto end;}}
        if(glGetError()!=GL_NO_ERROR) goto end;
        SDL_GL_SwapWindow(window);
        if(SDL_GetQueuedAudioSize(audio)<6400) {if(game.audio(pcm,800)!=0 || SDL_QueueAudio(audio,pcm,sizeof(pcm))!=0) goto end;}
        SDL_Delay(8);
    }
    if(active && game.pause()!=0) goto end;
    active=0;
    { FILE *save=fopen("score.txt.new","wx"); int ok;
      if(!save) goto end;
      ok=fprintf(save,"%d\n",game.save())>0; if(fclose(save)!=0) ok=0;
      if(!ok || rename("score.txt.new","score.txt")!=0) goto end; }
    result=0;
end:
    if(result) fprintf(stderr,"interactive demo failed: %s\n",SDL_GetError());
    if(audio) SDL_CloseAudioDevice(audio);
    if(pad) SDL_GameControllerClose(pad);
    if(active) game.pause();
    if(created) game.destroy();
    if(context) {if(texture)glDeleteTextures(1,&texture);if(program)glDeleteProgram(program);if(vertex)glDeleteShader(vertex);if(fragment)glDeleteShader(fragment);SDL_GL_DeleteContext(context);}
    if(window) SDL_DestroyWindow(window);
    SDL_Quit(); return result;
}
int main(int argc,char **argv) {
    nxloader_module *module=NULL;nxloader_registry *registry=NULL;
    nxloader_config config;nxloader_resolution_report resolved={.struct_size=sizeof(resolved)};
    nxloader_symbol symbols[]={ {"__android_log_write",(uintptr_t)android_log,0},{"__errno",(uintptr_t)android_errno,0} };
    nxloader_provider provider={sizeof(provider),"nextos-training",symbols,2,0};
    const int32_t versions[]={DEMO_JNI_VERSION};
    nxloader_jni_onload_options options={sizeof(options),&vm,NULL,versions,1,0};
    int result,omit=0;int32_t version=0;
    if(argc<2 || argc>3 || (argc==3 && strcmp(argv[2],"--self-test") && strcmp(argv[2],"--omit-log"))) {
        fprintf(stderr,"usage: first-port-nextos GAME_DIRECTORY [--self-test|--omit-log]\n"); return 1;
    }
    if(chdir(argv[1])!=0) {perror("game directory");return 1;}
    {FILE *seed=fopen("seed.txt","r");int value=0;if(!seed || fscanf(seed,"%d",&value)!=1 || value!=7) {if(seed)fclose(seed);fputs("FAIL prepared seed; run NXExtract first\n",stderr);return 1;}fclose(seed);}
    omit=argc==3 && !strcmp(argv[2],"--omit-log");
    owner=pthread_self();environment=&jni;
    nxloader_config_init(&config);config.expected_arch=NXLOADER_ARCH_AARCH64;config.log=loader_log;
    check(nxloader_module_create(&config,&module),"create");
    check(nxloader_registry_create(&registry),"registry");
    if(omit) {provider.symbols=&symbols[1];provider.symbol_count=1;}
    check(nxloader_registry_add_provider(registry,&provider,NULL),"providers");
    check(nxloader_module_load_file(module,"lib/arm64-v8a/libtraining.so"),"load");
    check(nxloader_module_relocate(module),"relocate");
    check(nxloader_module_resolve(module,registry,0,&resolved),"resolve");
    check(nxloader_module_finalize(module),"finalize");
    check(nxloader_module_call_initializers(module),"constructors");
    check(nxloader_module_call_jni_onload(module,&options,&version),"JNI_OnLoad");
    LOAD(create,"demo_create");LOAD(resume,"demo_resume");LOAD(step,"demo_step");LOAD(render,"demo_render");LOAD(audio,"demo_audio");LOAD(pause,"demo_pause");LOAD(save,"demo_save");LOAD(restore,"demo_restore");LOAD(destroy,"demo_destroy");
    result=argc==3 ? self_test() : interactive();
    nxloader_registry_destroy(registry);nxloader_module_destroy(module);return result;
}
