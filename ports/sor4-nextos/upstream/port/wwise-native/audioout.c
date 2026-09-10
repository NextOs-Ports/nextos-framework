// audioout.c — saida de audio CUSTOM (hibrido) p/ SoR4 no Mali-450.
//
// A Wwise NATIVA roda (silenciosa internamente) mas faz toda a LOGICA de audio:
// decide QUAL musica/wem tocar por contexto (menu/fase/chefe) e ABRE o .wem streamed.
// Aqui interceptamos isso e tocamos o som de verdade via OpenAL+opusfile (dlopen):
//   - MUSICA: a Wwise abre o .wem streamed (gameassets/NNN.wem) -> ao_music_request(path)
//     -> thread de streaming decodifica o chunk Ogg Opus e toca em LOOP.
//   - SFX (gui/confirmacao/in-bank): post_event(name) -> FNV-1 -> manifest -> <id>.opus
//     -> toca via OpenAL (buffers em cache).
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

// ---------------- OpenAL ----------------
typedef void ALCdevice; typedef void ALCcontext;
typedef int ALCboolean; typedef unsigned int ALuint; typedef int ALsizei; typedef int ALenum; typedef int ALint; typedef float ALfloat;
#define AL_FORMAT_MONO16   0x1101
#define AL_FORMAT_STEREO16 0x1103
#define AL_BUFFER          0x1009
#define AL_SOURCE_STATE    0x1010
#define AL_PLAYING         0x1012
#define AL_GAIN            0x100A
#define AL_LOOPING         0x1007
#define AL_BUFFERS_PROCESSED 0x1016
#define AL_BUFFERS_QUEUED    0x1015

static ALCdevice* (*p_alcOpenDevice)(const char*);
static ALCcontext*(*p_alcCreateContext)(ALCdevice*, const int*);
static ALCboolean (*p_alcMakeContextCurrent)(ALCcontext*);
static void (*p_alcDestroyContext)(ALCcontext*);
static ALCboolean (*p_alcCloseDevice)(ALCdevice*);
static void (*p_alGenSources)(ALsizei, ALuint*);
static void (*p_alDeleteSources)(ALsizei, const ALuint*);
static void (*p_alGenBuffers)(ALsizei, ALuint*);
static void (*p_alDeleteBuffers)(ALsizei, const ALuint*);
static void (*p_alBufferData)(ALuint, ALenum, const void*, ALsizei, ALsizei);
static void (*p_alSourcei)(ALuint, ALenum, ALint);
static void (*p_alSourcef)(ALuint, ALenum, ALfloat);
static void (*p_alSourcePlay)(ALuint);
static void (*p_alSourceStop)(ALuint);
static void (*p_alGetSourcei)(ALuint, ALenum, ALint*);
static void (*p_alSourceQueueBuffers)(ALuint, ALsizei, const ALuint*);
static void (*p_alSourceUnqueueBuffers)(ALuint, ALsizei, ALuint*);

// ---------------- opusfile ----------------
typedef void OggOpusFile;
static OggOpusFile* (*p_op_open_memory)(const unsigned char*, size_t, int*);
static int  (*p_op_read)(OggOpusFile*, int16_t*, int, int*);
static int  (*p_op_read_stereo)(OggOpusFile*, int16_t*, int);
static int  (*p_op_channel_count)(OggOpusFile*, int);
static int  (*p_op_pcm_seek)(OggOpusFile*, int64_t);
static void (*p_op_free)(OggOpusFile*);

static int g_ok = 0;
static char g_sfxdir[512] = "../audioout";
static ALCdevice* g_al_dev = NULL;
static ALCcontext* g_al_ctx = NULL;

static void aolog(const char* s){
  const char* lp=getenv("WWISE_LOG"); if(!lp||!*lp) lp="/tmp/sor4-wwise.log";
  FILE* f=fopen(lp,"a"); if(f){fprintf(f,"%s\n",s); fclose(f);}
}
static int aolog_verbose(void){
  static int v=-1;
  if(v<0){ const char* e=getenv("SOR4_NATLOG"); v=(e&&e[0]=='1')?1:0; }
  return v;
}

static int load_libs(void){
  void* h = dlopen("libopenal.so.1", RTLD_NOW|RTLD_GLOBAL); if(!h) h=dlopen("libopenal.so",RTLD_NOW|RTLD_GLOBAL);
  if(!h){ aolog("[audioout] dlopen libopenal FALHOU"); return 0; }
  p_alcOpenDevice=dlsym(h,"alcOpenDevice"); p_alcCreateContext=dlsym(h,"alcCreateContext");
  p_alcMakeContextCurrent=dlsym(h,"alcMakeContextCurrent");
  p_alcDestroyContext=dlsym(h,"alcDestroyContext"); p_alcCloseDevice=dlsym(h,"alcCloseDevice");
  p_alGenSources=dlsym(h,"alGenSources"); p_alGenBuffers=dlsym(h,"alGenBuffers");
  p_alDeleteSources=dlsym(h,"alDeleteSources");
  p_alDeleteBuffers=dlsym(h,"alDeleteBuffers");
  p_alBufferData=dlsym(h,"alBufferData"); p_alSourcei=dlsym(h,"alSourcei");
  p_alSourcef=dlsym(h,"alSourcef"); p_alSourcePlay=dlsym(h,"alSourcePlay");
  p_alSourceStop=dlsym(h,"alSourceStop"); p_alGetSourcei=dlsym(h,"alGetSourcei");
  p_alSourceQueueBuffers=dlsym(h,"alSourceQueueBuffers");
  p_alSourceUnqueueBuffers=dlsym(h,"alSourceUnqueueBuffers");
  void* o = dlopen("libopusfile.so.0", RTLD_NOW|RTLD_GLOBAL); if(!o) o=dlopen("libopusfile.so",RTLD_NOW|RTLD_GLOBAL);
  if(!o){ aolog("[audioout] dlopen libopusfile FALHOU"); return 0; }
  p_op_open_memory=dlsym(o,"op_open_memory"); p_op_read=dlsym(o,"op_read");
  p_op_read_stereo=dlsym(o,"op_read_stereo");
  p_op_channel_count=dlsym(o,"op_channel_count"); p_op_pcm_seek=dlsym(o,"op_pcm_seek");
  p_op_free=dlsym(o,"op_free");
  if(!p_alcOpenDevice||!p_alcCreateContext||!p_alcMakeContextCurrent||
     !p_alcDestroyContext||!p_alcCloseDevice||!p_alGenSources||!p_alDeleteSources||
     !p_alGenBuffers||!p_alDeleteBuffers||!p_op_open_memory||!p_op_read){
    aolog("[audioout] dlsym incompleto"); return 0;
  }
  return 1;
}

// Os ganhos SOR4_*GAIN sao a calibracao do port; os RTPCs do menu sao o volume
// escolhido pelo usuario. O mixer custom precisa aplicar os dois, pois o audio real
// da Wwise e usado apenas para logica/seleção e nao chega aos alto-falantes.
static pthread_mutex_t g_gain_mtx = PTHREAD_MUTEX_INITIALIZER;
static float g_sfx_user_gain=1.0f, g_music_user_gain=1.0f;
static float clamp_user_gain(float v){ if(v!=v||v<0.0f)return 0.0f; if(v>1.0f)return 1.0f; return v; }
static float user_gain(int music){
  float v; pthread_mutex_lock(&g_gain_mtx);
  v=music?g_music_user_gain:g_sfx_user_gain;
  pthread_mutex_unlock(&g_gain_mtx); return v;
}
static float sfx_base_gain(void){
  static int init=0; static float gain;
  if(!init){ const char* e=getenv("SOR4_SFXGAIN"); gain=(e&&*e)?(float)atof(e):0.55f; init=1; }
  return gain;
}
static float music_base_gain(void){
  static int init=0; static float gain;
  if(!init){ const char* e=getenv("SOR4_MUSICGAIN"); gain=(e&&*e)?(float)atof(e):0.7f; init=1; }
  return gain;
}
static float sfx_gain(void){ return sfx_base_gain()*user_gain(0); }
static float music_gain(void){ return music_base_gain()*user_gain(1); }

typedef struct {
  volatile unsigned long sfx_events;
  volatile unsigned long sfx_manifest_hits;
  volatile unsigned long sfx_played;
  volatile unsigned long sfx_decode_failed;
  volatile unsigned long sfx_manifest_misses;
  volatile unsigned long streamed_requests;
  volatile unsigned long streamed_played;
  volatile unsigned long streamed_failed;
  volatile unsigned long music_requests;
  volatile unsigned long music_markers;
  volatile unsigned long music_started;
} AudioStats;
static AudioStats g_stats;
static void stat_inc(volatile unsigned long* value){ (void)__sync_fetch_and_add(value,1); }

// =================== SFX (manifest -> .opus) ===================
typedef struct { uint32_t id; int ids[8]; int nids; int cur; } Entry;
static Entry* g_ent=NULL; static int g_nent=0, g_cap=0;
typedef struct { int id; ALuint buf; int valid; int bytes; unsigned last_use; } BufC;
static BufC* g_buf=NULL; static int g_nbuf=0, g_cbuf=0;
static unsigned g_use_clock=0;   // relogio p/ LRU
static long g_sfx_bytes=0;       // total de PCM cacheado (bytes)
#define NSFX 28
static ALuint g_sfx_src[NSFX];

// Recebe os RTPCs que BeatThemAll.audio.update envia a cada frame. So toca no
// OpenAL quando o valor realmente muda; assim o slider tambem altera efeitos que
// ja estao tocando sem recriar a fonte.
void ao_set_rtpc_volume(const char* name,float value){
  if(!name) return;
  int music=(strcmp(name,"MusicVolume")==0);
  if(!music && strcmp(name,"SfxVolume")!=0) return;
  value=clamp_user_gain(value);
  pthread_mutex_lock(&g_gain_mtx);
  float* target=music?&g_music_user_gain:&g_sfx_user_gain;
  int changed=(*target!=value);
  *target=value;
  pthread_mutex_unlock(&g_gain_mtx);
  if(!changed) return;
  if(!music && g_ok){
    float gain=sfx_gain();
    for(int i=0;i<NSFX;i++) if(g_sfx_src[i]) p_alSourcef(g_sfx_src[i],AL_GAIN,gain);
  }
  if(aolog_verbose()){
    char b[128]; snprintf(b,sizeof(b),"[audioout] RTPC %s=%.2f -> ganho OpenAL=%.3f",
                          name,value,music?music_gain():sfx_gain()); aolog(b);
  }
}
// Teto do cache de SFX em RAM. O round-robin entre variantes (cada golpe/queda/morte
// tem 6-8 wems) faz o cache crescer rapido; sem teto ele iria pra swap. LRU evicta o
// menos usado quando passa do limite. Default 24MB (centenas de SFX curtos cabem).
static long sfx_cache_limit(void){ const char* e=getenv("SOR4_SFXCACHE_MB"); long mb=(e&&*e)?atol(e):24; if(mb<2)mb=2; return mb*1024*1024; }

static uint32_t fnv1_32(const char* s){
  uint32_t h=2166136261u;
  for(; *s; ++s){ unsigned char c=(unsigned char)*s; if(c>='A'&&c<='Z') c+=32; h*=16777619u; h^=c; }
  return h;
}
static void load_manifest(void){
  char path[600]; snprintf(path,sizeof(path),"%s/manifest.txt",g_sfxdir);
  FILE* f=fopen(path,"r"); if(!f){ aolog("[audioout] manifest nao encontrado"); return; }
  char line[4096];
  while(fgets(line,sizeof(line),f)){
    char* tab=strchr(line,'\t'); if(!tab) continue; *tab=0;
    uint32_t eid=(uint32_t)strtoul(line,NULL,10); char* ids=tab+1;
    if(g_nent>=g_cap){ g_cap=g_cap?g_cap*2:1024; g_ent=realloc(g_ent,g_cap*sizeof(Entry)); }
    Entry* e=&g_ent[g_nent]; e->id=eid; e->nids=0; e->cur=0;
    char* p=ids; while(*p && e->nids<8){ int v=atoi(p); if(v>0) e->ids[e->nids++]=v; char* c=strchr(p,','); if(!c)break; p=c+1; }
    if(e->nids>0) g_nent++;
  }
  fclose(f);
  char b[128]; snprintf(b,sizeof(b),"[audioout] manifest: %d eventos SFX",g_nent); aolog(b);
}
// decodifica <id>.opus -> AL buffer (cache LRU com teto de RAM).
static ALuint sfx_buffer(int wem_id){
  // hit: marca uso (LRU). Entradas com valid=0 (decode falhou) ficam cacheadas como
  // "ausente" p/ nao re-tentar; entradas evictadas viram id=-1 (re-decodificam se voltarem).
  for(int i=0;i<g_nbuf;i++) if(g_buf[i].id==wem_id){ if(g_buf[i].valid) g_buf[i].last_use=++g_use_clock; return g_buf[i].valid? g_buf[i].buf : 0; }
  ALuint outbuf=0; int valid=0, nbytes=0;
  char path[600]; snprintf(path,sizeof(path),"%s/%d.opus",g_sfxdir,wem_id);
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
            int li=0; int r=p_op_read(of,pcm+n,cap-n,&li);
            if(r<=0) break; n += r*ch;
          }
          p_op_free(of);
          if(n>0){ p_alGenBuffers(1,&outbuf);
            p_alBufferData(outbuf, ch==1?AL_FORMAT_MONO16:AL_FORMAT_STEREO16, pcm, n*(int)sizeof(int16_t), 48000); valid=1; nbytes=n*(int)sizeof(int16_t); }
          free(pcm);
        }
      }
      free(fil);
    }
    fclose(f);
  }
  // EVICTA LRU ate caber (nao remove o que acabamos de criar).
  if(valid){
    long limit=sfx_cache_limit();
    while(g_sfx_bytes+nbytes>limit){
      int lru=-1; unsigned best=0xffffffffu;
      for(int i=0;i<g_nbuf;i++) if(g_buf[i].valid && g_buf[i].buf!=outbuf && g_buf[i].last_use<best){ best=g_buf[i].last_use; lru=i; }
      if(lru<0) break;
      if(g_buf[lru].buf) p_alDeleteBuffers(1,&g_buf[lru].buf);
      g_sfx_bytes-=g_buf[lru].bytes;
      g_buf[lru].id=-1; g_buf[lru].buf=0; g_buf[lru].valid=0; g_buf[lru].bytes=0;
      if(getenv("WWISE_TRACE")){ char b[96]; snprintf(b,sizeof(b),"[audioout] LRU evict (cache %ld KB)",g_sfx_bytes/1024); aolog(b); }
    }
  }
  // insere: reusa slot evictado (id==-1) se houver, senao cresce.
  int slot=-1;
  for(int i=0;i<g_nbuf;i++) if(g_buf[i].id==-1 && !g_buf[i].valid){ slot=i; break; }
  if(slot<0){ if(g_nbuf>=g_cbuf){ g_cbuf=g_cbuf?g_cbuf*2:512; g_buf=realloc(g_buf,g_cbuf*sizeof(BufC)); } slot=g_nbuf++; }
  g_buf[slot].id=wem_id; g_buf[slot].buf=outbuf; g_buf[slot].valid=valid; g_buf[slot].bytes=nbytes; g_buf[slot].last_use=++g_use_clock;
  if(valid) g_sfx_bytes+=nbytes;
  return valid? outbuf : 0;
}
static ALuint sfx_free_source(void){
  for(int i=0;i<NSFX;i++){ ALint st=0; p_alGetSourcei(g_sfx_src[i],AL_SOURCE_STATE,&st); if(st!=AL_PLAYING) return g_sfx_src[i]; }
  return g_sfx_src[0];
}
void ao_post_event(const char* name){
  if(!g_ok||!name) return;
  stat_inc(&g_stats.sfx_events);
  int trace=getenv("WWISE_TRACE")!=NULL;
  uint32_t eid=fnv1_32(name);
  for(int i=0;i<g_nent;i++){
    if(g_ent[i].id==eid){
      stat_inc(&g_stats.sfx_manifest_hits);
      Entry* e=&g_ent[i];
      // Varia a variante a cada disparo (round-robin): golpes/quedas soam diferentes
      // a cada vez; eventos de 1 wem (UI/voz) sempre tocam o mesmo. Comeca no cursor e
      // avanca; se a variante nao decodificar, tenta a proxima.
      int start=e->cur; e->cur=(e->cur+1)%(e->nids>0?e->nids:1);
      for(int j=0;j<e->nids;j++){
        int k=(start+j)%e->nids;
        ALuint buf=sfx_buffer(e->ids[k]);
        if(buf){ ALuint src=sfx_free_source();
          p_alSourceStop(src); p_alSourcei(src,AL_BUFFER,(ALint)buf);
          p_alSourcef(src,AL_GAIN,sfx_gain()); p_alSourcei(src,AL_LOOPING,0); p_alSourcePlay(src);
          stat_inc(&g_stats.sfx_played);
          if(trace){ char b[160]; snprintf(b,sizeof(b),"[audioout] SFX OK '%s' wem=%d (var %d/%d)",name,e->ids[k],k+1,e->nids); aolog(b); }
          return; }
      }
      stat_inc(&g_stats.sfx_decode_failed);
      if(trace){ char b[200]; snprintf(b,sizeof(b),"[audioout] SFX '%s' achado no manifest (%d wem) mas NENHUM .opus decodificou",name,e->nids); aolog(b); }
      return;
    }
  }
  stat_inc(&g_stats.sfx_manifest_misses);
  if(trace){ char b[160]; snprintf(b,sizeof(b),"[audioout] SFX '%s' NAO no manifest (eid=%u)",name,eid); aolog(b); }
}

// =================== MUSICA (wem streamed -> loop) ===================
// Acha o chunk "data" (Ogg Opus) dentro do .wem (RIFF/WAVE codec 0x3040).
static int wem_find_data(const unsigned char* buf, long sz, long* off, long* len){
  if(sz<44 || memcmp(buf,"RIFF",4)!=0 || memcmp(buf+8,"WAVE",4)!=0) return 0;
  long p=12;
  while(p+8<=sz){
    const unsigned char* c=buf+p;
    uint32_t csz = c[4]|(c[5]<<8)|(c[6]<<16)|((uint32_t)c[7]<<24);
    if(memcmp(c,"data",4)==0){ *off=p+8; *len=(long)csz; if(*off+*len>sz)*len=sz-*off; return 1; }
    p += 8 + csz + (csz&1);
  }
  return 0;
}

static pthread_mutex_t g_mus_mtx = PTHREAD_MUTEX_INITIALIZER;
static char g_mus_pending[1024] = "";   // path pedido (ou "" p/ parar)
static char g_mus_current[1024] = "";   // path tocando agora
static double g_mus_stop_at = 0;        // monotonic s: parar SE chegar aqui sem nova musica (0=nunca)
static volatile int g_mus_run = 0;
static pthread_t g_mus_thread;
static int g_mus_thread_started = 0;
static ALuint g_mus_src = 0;

// MUSICA por STREAMING (buffer-queue OpenAL) com FOLGA GRANDE p/ aguentar picos de
// CPU do combate sem stutter. A musica faz loop re-abrindo/seekando o Opus no fim.
#define MUS_NBUF 24        // ~8s bufferizado (24 x 16384/48000) -> cobre picos de combate
#define MUS_FRAMES 16384   // samples/canal por buffer

static double now_s(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec + ts.tv_nsec/1e9; }
static double music_grace(void){ const char* e=getenv("SOR4_MUSIC_GRACE"); return (e&&*e)?atof(e):8.0; }

// ---- SFX STREAMED (one-shot): .wem PEQUENO que a Wwise abre = efeito (hit/voz/stinger),
// NAO musica. Decodifica inteiro (curto), cacheia por path, toca uma vez. Assim sons
// streamed de combate TOCAM e nao SEQUESTRAM a musica de fundo. ----
static int decode_wem_to_albuf(const char* path, ALuint outbuf, int* out_ch){
  FILE* f=fopen(path,"rb"); if(!f) return 0;
  fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
  int total=0, ch=2;
  if(sz>0){
    unsigned char* fil=malloc(sz);
    if(fil && fread(fil,1,sz,f)==(size_t)sz){
      long off=0,len=0;
      if(wem_find_data(fil,sz,&off,&len)){
        int err=0; OggOpusFile* of=p_op_open_memory(fil+off,len,&err);
        if(of){ ch=p_op_channel_count(of,-1); if(ch<1)ch=1; if(ch>2)ch=2;
          const int MAXN=48000*ch*30; int cap=48000*ch*2, n=0; int16_t* pcm=malloc(cap*sizeof(int16_t));
          for(;;){ if(n+11520*ch>cap){ cap*=2; int16_t* np=realloc(pcm,cap*sizeof(int16_t)); if(!np)break; pcm=np; }
            int li=0; int r=p_op_read(of,pcm+n,cap-n,&li); if(r<=0)break; n+=r*ch; if(n>=MAXN)break; }
          p_op_free(of);
          if(n>0){ p_alBufferData(outbuf, ch==1?AL_FORMAT_MONO16:AL_FORMAT_STEREO16, pcm, n*(int)sizeof(int16_t), 48000); total=n; }
          free(pcm);
        }
      }
    }
    free(fil);
  }
  fclose(f);
  if(out_ch)*out_ch=ch;
  return total;
}
typedef struct { char path[1024]; ALuint buf; int valid; } StrSfx;
static StrSfx* g_ssfx=NULL; static int g_nssfx=0,g_cssfx=0;
void ao_play_streamed_sfx(const char* path){
  if(!g_ok||!path) return;
  stat_inc(&g_stats.streamed_requests);
  ALuint buf=0; int found=0;
  for(int i=0;i<g_nssfx;i++) if(strcmp(g_ssfx[i].path,path)==0){ buf=g_ssfx[i].valid?g_ssfx[i].buf:0; found=1; break; }
  if(!found){
    ALuint nb=0; p_alGenBuffers(1,&nb); int ch=2; int n=decode_wem_to_albuf(path,nb,&ch); int valid=n>0;
    if(!valid){ p_alDeleteBuffers(1,&nb); nb=0; }
    if(g_nssfx>=g_cssfx){ g_cssfx=g_cssfx?g_cssfx*2:128; g_ssfx=realloc(g_ssfx,g_cssfx*sizeof(StrSfx)); }
    strncpy(g_ssfx[g_nssfx].path,path,sizeof(g_ssfx[g_nssfx].path)-1); g_ssfx[g_nssfx].path[sizeof(g_ssfx[g_nssfx].path)-1]=0;
    g_ssfx[g_nssfx].buf=nb; g_ssfx[g_nssfx].valid=valid; g_nssfx++;
    buf=valid?nb:0;
    if(getenv("WWISE_TRACE")){ char b[1100]; snprintf(b,sizeof(b),"[audioout] STREAMED-SFX %s -> %s",path,valid?"OK":"FALHOU"); aolog(b); }
  }
  if(buf){ ALuint src=sfx_free_source(); p_alSourceStop(src); p_alSourcei(src,AL_BUFFER,(ALint)buf);
    p_alSourcef(src,AL_GAIN,sfx_gain()); p_alSourcei(src,AL_LOOPING,0); p_alSourcePlay(src);
    stat_inc(&g_stats.streamed_played);
  } else {
    stat_inc(&g_stats.streamed_failed);
  }
}

// le ate MUS_FRAMES frames; no EOF, loopa (op_pcm_seek 0). retorna frames lidos.
static int mus_fill(OggOpusFile* of, int16_t* tmp, int ch, unsigned char* filbuf, long off, long len, OggOpusFile** pof){
  int got=0;
  while(got<MUS_FRAMES){
    int li=0; int r=p_op_read(of,tmp+got*ch,(MUS_FRAMES-got)*ch,&li);
    if(r<=0){ if(p_op_pcm_seek){ if(p_op_pcm_seek(of,0)!=0) break; } else { p_op_free(of); int e=0; of=p_op_open_memory(filbuf+off,len,&e); *pof=of; if(!of) break; } continue; }
    got+=r;
  }
  return got;
}

static void* music_thread(void* a){ (void)a;
  unsigned char* filbuf=NULL; OggOpusFile* of=NULL; long dataoff=0,datalen=0; int ch=2;
  ALuint bufs[MUS_NBUF]; int bufs_gen=0;
  int16_t* tmp=malloc(MUS_FRAMES*2*sizeof(int16_t));
  char playing[1024]="";
  float applied_music_gain=-1.0f;
  while(g_mus_run){
    char want[1024];
    pthread_mutex_lock(&g_mus_mtx);
    // periodo de graca: a Wwise fechou o wem mas nao abriu outro -> so paramos de
    // verdade se passou a graca SEM nova musica (cobre gaps de musica interativa).
    if(g_mus_stop_at>0 && now_s()>=g_mus_stop_at && g_mus_pending[0] && strcmp(g_mus_pending,g_mus_current)==0){ g_mus_pending[0]=0; g_mus_stop_at=0; }
    strncpy(want,g_mus_pending,sizeof(want)-1); want[sizeof(want)-1]=0;
    pthread_mutex_unlock(&g_mus_mtx);
    if(strcmp(want,playing)!=0){
      if(g_mus_src){ p_alSourceStop(g_mus_src); p_alSourcei(g_mus_src,AL_BUFFER,0); }
      if(of){ p_op_free(of); of=NULL; }
      if(filbuf){ free(filbuf); filbuf=NULL; }
      playing[0]=0;
      pthread_mutex_lock(&g_mus_mtx); g_mus_current[0]=0; pthread_mutex_unlock(&g_mus_mtx);
      if(want[0]){
        FILE* f=fopen(want,"rb");
        if(f){ fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
          if(sz>0){ filbuf=malloc(sz);
            if(filbuf && fread(filbuf,1,sz,f)==(size_t)sz && wem_find_data(filbuf,sz,&dataoff,&datalen)){
              int err=0; of=p_op_open_memory(filbuf+dataoff,datalen,&err);
              if(of){ ch=p_op_channel_count(of,-1); if(ch<1)ch=1; if(ch>2)ch=2;
                if(!bufs_gen){ p_alGenBuffers(MUS_NBUF,bufs); bufs_gen=1; }
                if(!g_mus_src) p_alGenSources(1,&g_mus_src);
                float gain=music_gain();
                p_alSourcei(g_mus_src,AL_LOOPING,0); p_alSourcef(g_mus_src,AL_GAIN,gain);
                applied_music_gain=gain;
                int queued=0;
                for(int b=0;b<MUS_NBUF;b++){
                  int got=mus_fill(of,tmp,ch,filbuf,dataoff,datalen,&of); if(got<=0) break;
                  p_alBufferData(bufs[b], ch==1?AL_FORMAT_MONO16:AL_FORMAT_STEREO16, tmp, got*ch*(int)sizeof(int16_t), 48000);
                  p_alSourceQueueBuffers(g_mus_src,1,&bufs[b]); queued++;
                }
                if(queued>0){ p_alSourcePlay(g_mus_src); strncpy(playing,want,sizeof(playing)-1);
                  pthread_mutex_lock(&g_mus_mtx); strncpy(g_mus_current,want,sizeof(g_mus_current)-1); pthread_mutex_unlock(&g_mus_mtx);
                  stat_inc(&g_stats.music_started);
                  { char b[1100]; snprintf(b,sizeof(b),"[audioout] MUSICA tocando %s (ch=%d, streaming %d bufs)",want,ch,queued); aolog(b); }
                } else { p_op_free(of); of=NULL; free(filbuf); filbuf=NULL; }
              } else { free(filbuf); filbuf=NULL; }
            } else { if(filbuf){free(filbuf);filbuf=NULL;} }
          }
          fclose(f);
        }
      }
    }
    // A fonte de musica pertence a esta thread; atualiza o slider aqui para nao
    // disputar a mesma AL source com a thread do jogo.
    if(g_mus_src){
      float gain=music_gain();
      if(gain!=applied_music_gain){ p_alSourcef(g_mus_src,AL_GAIN,gain); applied_music_gain=gain; }
    }
    // streaming: recicla buffers ja tocados (refill)
    if(of && playing[0] && g_mus_src){
      ALint proc=0; p_alGetSourcei(g_mus_src,AL_BUFFERS_PROCESSED,&proc);
      while(proc-->0){
        ALuint b=0; p_alSourceUnqueueBuffers(g_mus_src,1,&b);
        int got=mus_fill(of,tmp,ch,filbuf,dataoff,datalen,&of); if(got<=0){ break; }
        p_alBufferData(b, ch==1?AL_FORMAT_MONO16:AL_FORMAT_STEREO16, tmp, got*ch*(int)sizeof(int16_t), 48000);
        p_alSourceQueueBuffers(g_mus_src,1,&b);
      }
      ALint st=0; p_alGetSourcei(g_mus_src,AL_SOURCE_STATE,&st);
      ALint q=0; p_alGetSourcei(g_mus_src,AL_BUFFERS_QUEUED,&q);
      if(st!=AL_PLAYING && q>0) p_alSourcePlay(g_mus_src);
    }
    usleep(25000);   // ~40Hz: leve; com 8s de folga sobra tempo de refil
  }
  if(g_mus_src){
    p_alSourceStop(g_mus_src);
    ALint q=0; p_alGetSourcei(g_mus_src,AL_BUFFERS_QUEUED,&q);
    while(q-->0){ ALuint b=0; p_alSourceUnqueueBuffers(g_mus_src,1,&b); }
    p_alSourcei(g_mus_src,AL_BUFFER,0);
    p_alDeleteSources(1,&g_mus_src);
    g_mus_src=0;
  }
  if(bufs_gen) p_alDeleteBuffers(MUS_NBUF,bufs);
  if(of) p_op_free(of);
  free(filbuf);
  free(tmp);
  return NULL;
}

// Alguns MusicSegments da Wwise sao marcadores temporais: WEMs minimos com dezenas
// de segundos de PCM silencioso. Se eles substituirem a fonte unica, a musica valida
// desaparece ate o proximo segmento (notavelmente nas sirenes da fase da prisao).
// So sondamos arquivos compactados <=64 KiB; falha de leitura/decode conserva o
// comportamento original. Menos de 0,1% de amostras acima de +/-64 e inaudivel e
// deve manter a musica que ja esta tocando.
static int music_payload_is_audible(const char* path){
  FILE* f=fopen(path,"rb"); if(!f) return 1;
  fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
  if(sz<=0 || sz>65536){ fclose(f); return 1; }
  unsigned char* fil=malloc((size_t)sz);
  if(!fil || fread(fil,1,(size_t)sz,f)!=(size_t)sz){ free(fil); fclose(f); return 1; }
  fclose(f);
  long off=0,len=0; int audible=1;
  if(wem_find_data(fil,sz,&off,&len)){
    int err=0; OggOpusFile* of=p_op_open_memory(fil+off,(size_t)len,&err);
    if(of){
      int ch=p_op_channel_count(of,-1); if(ch<1)ch=1; if(ch>2)ch=2;
      int16_t pcm[11520*2]; uint64_t total=0,active=0;
      for(;;){
        int li=0; int frames=p_op_read(of,pcm,(int)(sizeof(pcm)/sizeof(pcm[0])),&li);
        if(frames<=0) break;
        int samples=frames*ch;
        total+=(uint64_t)samples;
        for(int i=0;i<samples;i++) if(pcm[i]>64 || pcm[i]<-64) active++;
      }
      p_op_free(of);
      if(total>0 && active<(total+999u)/1000u) audible=0;
    }
  }
  free(fil);
  return audible;
}

// pede p/ a thread de musica tocar este .wem (ou "" p/ parar). Retorna 0 quando
// o WEM e um marcador silencioso e a faixa atual deve continuar.
int ao_music_request(const char* path){
  if(!g_ok) return 0;
  stat_inc(&g_stats.music_requests);
  if(path && *path && !music_payload_is_audible(path)){
    stat_inc(&g_stats.music_markers);
    char b[1100]; snprintf(b,sizeof(b),"[audioout] marcador musical silencioso ignorado: %s",path); aolog(b);
    return 0;
  }
  pthread_mutex_lock(&g_mus_mtx);
  strncpy(g_mus_pending, path?path:"", sizeof(g_mus_pending)-1); g_mus_pending[sizeof(g_mus_pending)-1]=0;
  g_mus_stop_at=0;   // nova musica pedida -> cancela qualquer parada agendada
  pthread_mutex_unlock(&g_mus_mtx);
  return 1;
}
// a Wwise FECHOU este .wem. Em musica INTERATIVA ela fecha/reabre segmentos toda hora;
// parar na hora dava SILENCIO no meio da fase. Entao AGENDAMOS a parada p/ daqui a uns
// segundos: se outra musica abrir antes (transicao normal), a parada e' cancelada e a
// musica nunca cai; se ninguem abrir (musica realmente acabou), paramos apos a graca.
void ao_music_close(const char* path){
  if(!g_ok||!path) return;
  pthread_mutex_lock(&g_mus_mtx);
  if(g_mus_current[0] && strcmp(path,g_mus_current)==0 && strcmp(path,g_mus_pending)==0 && g_mus_stop_at==0){
    g_mus_stop_at = now_s() + music_grace();
  }
  pthread_mutex_unlock(&g_mus_mtx);
}

// =================== init ===================
void ao_init(void){
  if(g_ok) return;
  memset((void*)&g_stats,0,sizeof(g_stats));
  const char* d=getenv("SOR4_AUDIO"); if(d&&*d) strncpy(g_sfxdir,d,sizeof(g_sfxdir)-1);
  if(!load_libs()){ aolog("[audioout] libs FALHOU -> sem audio custom"); return; }
  g_al_dev=p_alcOpenDevice(NULL); if(!g_al_dev){ aolog("[audioout] alcOpenDevice NULL"); return; }
  g_al_ctx=p_alcCreateContext(g_al_dev,NULL);
  if(!g_al_ctx){ aolog("[audioout] ctx NULL"); p_alcCloseDevice(g_al_dev); g_al_dev=NULL; return; }
  if(!p_alcMakeContextCurrent(g_al_ctx)){
    aolog("[audioout] alcMakeContextCurrent FALHOU");
    p_alcDestroyContext(g_al_ctx); p_alcCloseDevice(g_al_dev);
    g_al_ctx=NULL; g_al_dev=NULL; return;
  }
  p_alGenSources(NSFX,g_sfx_src);
  load_manifest();
  g_ok=1;
  if(!g_mus_thread_started){
    g_mus_run=1;
    if(pthread_create(&g_mus_thread,NULL,music_thread,NULL)==0) g_mus_thread_started=1;
    else { g_mus_run=0; aolog("[audioout] thread de musica FALHOU"); }
  }
  aolog("[audioout] init OK (OpenAL+opusfile, SFX manifest + musica streaming)");
}

// Fecha explicitamente o contexto antes do destructor do OpenAL Soft. Sem isso o
// OpenAL 1.19 de alguns CFW aborta no encerramento (alc_cleanup/LockLists), mesmo
// depois de uma sessao perfeitamente estavel.
void ao_shutdown(void){
  if(!g_ok && !g_al_ctx && !g_al_dev) return;
  g_ok=0;                         // impede novos eventos durante o teardown
  if(g_mus_thread_started){
    g_mus_run=0;
    pthread_join(g_mus_thread,NULL);
    g_mus_thread_started=0;
  }
  {
    char b[384];
    snprintf(b,sizeof(b),
      "[audioout] resumo: sfx req=%lu manifest=%lu tocados=%lu decode_fail=%lu ausentes=%lu; "
      "streamed req=%lu tocados=%lu falhas=%lu; musica req=%lu iniciadas=%lu marcadores=%lu",
      g_stats.sfx_events,g_stats.sfx_manifest_hits,g_stats.sfx_played,
      g_stats.sfx_decode_failed,g_stats.sfx_manifest_misses,
      g_stats.streamed_requests,g_stats.streamed_played,g_stats.streamed_failed,
      g_stats.music_requests,g_stats.music_started,g_stats.music_markers);
    aolog(b);
  }

  if(g_al_ctx) p_alcMakeContextCurrent(g_al_ctx);
  for(int i=0;i<NSFX;i++){
    if(g_sfx_src[i]){ p_alSourceStop(g_sfx_src[i]); p_alSourcei(g_sfx_src[i],AL_BUFFER,0); }
  }
  p_alDeleteSources(NSFX,g_sfx_src);
  memset(g_sfx_src,0,sizeof(g_sfx_src));

  for(int i=0;i<g_nbuf;i++) if(g_buf[i].valid && g_buf[i].buf) p_alDeleteBuffers(1,&g_buf[i].buf);
  for(int i=0;i<g_nssfx;i++) if(g_ssfx[i].valid && g_ssfx[i].buf) p_alDeleteBuffers(1,&g_ssfx[i].buf);
  free(g_buf); g_buf=NULL; g_nbuf=0; g_cbuf=0; g_sfx_bytes=0;
  free(g_ssfx); g_ssfx=NULL; g_nssfx=0; g_cssfx=0;
  free(g_ent); g_ent=NULL; g_nent=0; g_cap=0;

  if(g_al_ctx){
    p_alcMakeContextCurrent(NULL);
    p_alcDestroyContext(g_al_ctx);
    g_al_ctx=NULL;
  }
  if(g_al_dev){ p_alcCloseDevice(g_al_dev); g_al_dev=NULL; }
  aolog("[audioout] shutdown OK (OpenAL fechado limpo)");
}
