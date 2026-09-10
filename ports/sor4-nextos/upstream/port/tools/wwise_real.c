// wwise_real.c -> libWwise.so REAL (aarch64) p/ SoR4 no Mali-450.
// post_event(name) -> FNV-1 hash -> event_id -> wem_ids (manifest gerado offline) ->
// decodifica <id>.opus (chunk Ogg Opus do wem 0x3040) via libopusfile -> toca via OpenAL.
// Tudo via dlopen (libopenal.so.1 + libopusfile.so.0) -> PulseAudio no device. Sem deps de link.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

typedef void* ALCdevice; typedef void* ALCcontext;
typedef int ALCboolean; typedef unsigned int ALuint; typedef int ALsizei; typedef int ALenum; typedef int ALint; typedef float ALfloat;
#define AL_FORMAT_MONO16   0x1101
#define AL_FORMAT_STEREO16 0x1103
#define AL_BUFFER          0x1009
#define AL_SOURCE_STATE    0x1010
#define AL_PLAYING         0x1012
#define AL_GAIN            0x100A
#define AL_LOOPING         0x1007

static ALCdevice* (*p_alcOpenDevice)(const char*);
static ALCcontext*(*p_alcCreateContext)(ALCdevice*, const int*);
static ALCboolean (*p_alcMakeContextCurrent)(ALCcontext*);
static void (*p_alGenSources)(ALsizei, ALuint*);
static void (*p_alGenBuffers)(ALsizei, ALuint*);
static void (*p_alBufferData)(ALuint, ALenum, const void*, ALsizei, ALsizei);
static void (*p_alSourcei)(ALuint, ALenum, ALint);
static void (*p_alSourcef)(ALuint, ALenum, ALfloat);
static void (*p_alSourcePlay)(ALuint);
static void (*p_alSourceStop)(ALuint);
static void (*p_alGetSourcei)(ALuint, ALenum, ALint*);

// libopusfile
typedef void OggOpusFile;
static OggOpusFile* (*p_op_open_memory)(const unsigned char*, size_t, int*);
static int  (*p_op_read)(OggOpusFile*, int16_t*, int, int*);
static int  (*p_op_channel_count)(OggOpusFile*, int);
static void (*p_op_free)(OggOpusFile*);

static int g_ok = 0;
#define NSRC 48
static ALuint g_src[NSRC];
static char g_dir[512] = "/storage/roms/sor4-test/audioout";

typedef struct { uint32_t id; int ids[6]; int nids; } Entry;
static Entry* g_ent = NULL; static int g_nent = 0, g_cap = 0;
typedef struct { int id; ALuint buf; int valid; } BufC;
static BufC* g_buf = NULL; static int g_nbuf = 0, g_cbuf = 0;

static void log_line(const char* s){
  const char* lp=getenv("WWISE_LOG"); if(!lp||!*lp) lp="/storage/roms/sor4-test/wwise.log";
  FILE* f=fopen(lp,"a"); if(f){fprintf(f,"%s\n",s); fclose(f);}
}

static uint32_t fnv1_32(const char* s){
  uint32_t h=2166136261u;
  for(; *s; ++s){ unsigned char c=(unsigned char)*s; if(c>='A'&&c<='Z') c+=32; h*=16777619u; h^=c; }
  return h;
}

static int load_libs(void){
  void* h = dlopen("libopenal.so.1", RTLD_NOW|RTLD_GLOBAL); if(!h) h=dlopen("libopenal.so",RTLD_NOW|RTLD_GLOBAL);
  if(!h){ log_line("[wwise] dlopen libopenal FALHOU"); return 0; }
  p_alcOpenDevice=dlsym(h,"alcOpenDevice"); p_alcCreateContext=dlsym(h,"alcCreateContext");
  p_alcMakeContextCurrent=dlsym(h,"alcMakeContextCurrent");
  p_alGenSources=dlsym(h,"alGenSources"); p_alGenBuffers=dlsym(h,"alGenBuffers");
  p_alBufferData=dlsym(h,"alBufferData"); p_alSourcei=dlsym(h,"alSourcei");
  p_alSourcef=dlsym(h,"alSourcef"); p_alSourcePlay=dlsym(h,"alSourcePlay");
  p_alSourceStop=dlsym(h,"alSourceStop"); p_alGetSourcei=dlsym(h,"alGetSourcei");
  void* o = dlopen("libopusfile.so.0", RTLD_NOW|RTLD_GLOBAL); if(!o) o=dlopen("libopusfile.so",RTLD_NOW|RTLD_GLOBAL);
  if(!o){ log_line("[wwise] dlopen libopusfile FALHOU"); return 0; }
  p_op_open_memory=dlsym(o,"op_open_memory"); p_op_read=dlsym(o,"op_read");
  p_op_channel_count=dlsym(o,"op_channel_count"); p_op_free=dlsym(o,"op_free");
  if(!p_alcOpenDevice||!p_alGenSources||!p_op_open_memory||!p_op_read){ log_line("[wwise] dlsym incompleto"); return 0; }
  return 1;
}

static void load_manifest(void){
  char path[600]; snprintf(path,sizeof(path),"%s/manifest.txt",g_dir);
  FILE* f=fopen(path,"r"); if(!f){ log_line("[wwise] manifest nao encontrado"); return; }
  char line[4096];
  while(fgets(line,sizeof(line),f)){
    char* tab=strchr(line,'\t'); if(!tab) continue; *tab=0;
    uint32_t eid=(uint32_t)strtoul(line,NULL,10); char* ids=tab+1;
    if(g_nent>=g_cap){ g_cap=g_cap?g_cap*2:1024; g_ent=realloc(g_ent,g_cap*sizeof(Entry)); }
    Entry* e=&g_ent[g_nent]; e->id=eid; e->nids=0;
    char* p=ids; while(*p && e->nids<6){ int v=atoi(p); if(v>0) e->ids[e->nids++]=v; char* c=strchr(p,','); if(!c)break; p=c+1; }
    if(e->nids>0) g_nent++;
  }
  fclose(f);
  char b[128]; snprintf(b,sizeof(b),"[wwise] manifest: %d eventos com SFX",g_nent); log_line(b);
}

// decodifica <id>.opus -> AL buffer (cache). retorna buffer ou 0.
static ALuint get_buffer(int wem_id){
  for(int i=0;i<g_nbuf;i++) if(g_buf[i].id==wem_id) return g_buf[i].valid? g_buf[i].buf : 0;
  ALuint outbuf=0; int valid=0;
  char path[600]; snprintf(path,sizeof(path),"%s/%d.opus",g_dir,wem_id);
  FILE* f=fopen(path,"rb");
  if(f){
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    if(sz>0){
      unsigned char* fil=malloc(sz);
      if(fread(fil,1,sz,f)==(size_t)sz){
        int err=0; OggOpusFile* of=p_op_open_memory(fil,sz,&err);
        if(of){
          int ch=p_op_channel_count(of,-1); if(ch<1)ch=2; if(ch>2)ch=2;
          int cap=48000*4, n=0; int16_t* pcm=malloc(cap*sizeof(int16_t));
          for(;;){
            if(n+11520*ch > cap){ cap*=2; pcm=realloc(pcm,cap*sizeof(int16_t)); }
            int li=0; int r=p_op_read(of,pcm+n,cap-n,&li); // r=samples/canal
            if(r<=0) break; n += r*ch;
          }
          p_op_free(of);
          if(n>0){
            p_alGenBuffers(1,&outbuf);
            p_alBufferData(outbuf, ch==1?AL_FORMAT_MONO16:AL_FORMAT_STEREO16, pcm, n*(int)sizeof(int16_t), 48000);
            valid=1;
          }
          free(pcm);
        }
      }
      free(fil);
    }
    fclose(f);
  }
  if(g_nbuf>=g_cbuf){ g_cbuf=g_cbuf?g_cbuf*2:512; g_buf=realloc(g_buf,g_cbuf*sizeof(BufC)); }
  g_buf[g_nbuf].id=wem_id; g_buf[g_nbuf].buf=outbuf; g_buf[g_nbuf].valid=valid; g_nbuf++;
  return valid? outbuf : 0;
}

static ALuint free_source(void){
  for(int i=0;i<NSRC;i++){ ALint st=0; p_alGetSourcei(g_src[i],AL_SOURCE_STATE,&st); if(st!=AL_PLAYING) return g_src[i]; }
  return g_src[0];
}

int native_wwise_init(const char* soundbankPath){
  const char* d=getenv("SOR4_AUDIO"); if(d&&*d) strncpy(g_dir,d,sizeof(g_dir)-1);
  log_line("[wwise] init");
  if(!load_libs()) return 0;
  ALCdevice* dev=p_alcOpenDevice(NULL); if(!dev){ log_line("[wwise] alcOpenDevice NULL"); return 0; }
  ALCcontext* ctx=p_alcCreateContext(dev,NULL); if(!ctx){ log_line("[wwise] ctx NULL"); return 0; }
  p_alcMakeContextCurrent(ctx);
  p_alGenSources(NSRC,g_src);
  load_manifest();
  g_ok=1; log_line("[wwise] init OK (OpenAL+opusfile ativos)");
  return 1;
}
void native_wwise_destroy(void){}
void native_wwise_update(void){}
int  native_wwise_loadbank(const char* name){ return 1; }
int  native_wwise_unloadbank(const char* name){ return 1; }
void native_wwise_register_gameobject_with_id(uint64_t id){}
void native_wwise_register_gameobject_with_id_and_name(uint64_t id,const char* n){}
void native_wwise_set_switch(const char* g,const char* s,uint64_t o){}
void native_wwise_set_state(const char* g,const char* s){}

int native_wwise_post_event(const char* eventName){
  if(!eventName) return 0;
  if(getenv("WWISE_TRACE")){ char b[200]; snprintf(b,sizeof(b),"[wwise] POST '%s'",eventName); log_line(b); }
  if(!g_ok) return 0;
  uint32_t eid=fnv1_32(eventName);
  for(int i=0;i<g_nent;i++){
    if(g_ent[i].id==eid){
      Entry* e=&g_ent[i];
      // toca o 1o wem com opus decodificavel (RanSeq: uma variante; direto: o som exato)
      for(int k=0;k<e->nids;k++){
        ALuint buf=get_buffer(e->ids[k]);
        if(buf){
          ALuint src=free_source();
          p_alSourceStop(src); p_alSourcei(src,AL_BUFFER,(ALint)buf);
          p_alSourcef(src,AL_GAIN,1.0f); p_alSourcei(src,AL_LOOPING,0);
          p_alSourcePlay(src);
          { char b[160]; snprintf(b,sizeof(b),"[wwise] PLAY '%s' wem=%d",eventName,e->ids[k]); log_line(b); }
          return 1;
        }
      }
      return 0;
    }
  }
  return 0;
}
int native_wwise_post_event_with_id(const char* eventName, uint64_t gameObjectId){ return native_wwise_post_event(eventName); }
int native_wwise_post_trigger(const char* t){ return 0; }
int native_wwise_post_trigger_with_id(const char* t, uint64_t o){ return 0; }
int native_wwise_set_listener_position(void* v){ return 1; }
int native_wwise_set_gameobject_position(uint64_t o, void* v){ return 1; }
int native_wwise_set_rtpc_value(const char* n, float v){ return 1; }
int native_wwise_set_rtpc_value_with_id(const char* n, float v, uint64_t o){ return 1; }
int native_wwise_get_music_event(void* ev){ return 0; }
int native_wwise_get_total_memory(void){ return 0; }
int native_wwise_is_game_object_active(uint64_t o){ return 0; }
void native_wwise_unregister_inactive_game_objects(void){}
long get_total_memory_stub(void){ return 0; }
long native_android_preinit(void){ return 0; }
long sor4_gl_noop(void){ return 0; }
