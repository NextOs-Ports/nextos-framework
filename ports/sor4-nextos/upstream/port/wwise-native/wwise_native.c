// wwise_native.c — wrapper glibc libWwise.so (PLUGIN do .NET) que so-carrega a
// libWwise REAL do APK (motor Wwise nativo) e faz trampolim dos native_wwise_*.
// Construtor: so_load + relocate + resolve(tabela combinada) + init_array.
// Resolve os imports que faltavam (libc/libm glibc, _chk bionic_shims, __sF, AAsset, liblog).
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <math.h>
#include <syslog.h>
#include <sched.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/auxv.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <execinfo.h>
#include <ucontext.h>
#include "so_util.h"
extern void opensles_shim_pump_callbacks(void);   // opensles_shim.c: dispara o BufferQueue cb da Wwise
// audioout.c — saida de audio CUSTOM (hibrido): SFX via manifest, musica via wem streamed
extern void ao_init(void);
extern void ao_shutdown(void);
extern void ao_post_event(const char* name);
extern int ao_music_request(const char* path);
extern void ao_music_close(const char* path);
extern void ao_play_streamed_sfx(const char* path);   // .wem pequeno = efeito one-shot
extern void ao_set_rtpc_volume(const char* name,float value);

extern DynLibFunction dynlib_functions[];
extern size_t dynlib_numfunctions;
extern void jni_shim_init(void**, void**);   // jni_shim.c: JavaVM/JNIEnv fake
static void* g_fake_vm=NULL; static void* g_fake_env=NULL;

// bionic_shims.c (funcoes reais)
extern void  *__memcpy_chk(void*,const void*,size_t,size_t);
extern void  *__memmove_chk(void*,const void*,size_t,size_t);
extern size_t __strlen_chk(const char*,size_t);
extern char  *__strncat_chk(char*,const char*,size_t,size_t);
extern char  *__strncpy_chk(char*,const char*,size_t,size_t);
extern char  *__strncpy_chk2(char*,const char*,size_t,size_t,size_t);
extern int    __vsnprintf_chk(char*,size_t,int,size_t,const char*,va_list);
extern size_t __system_property_get(const char*, char*);
extern void   android_set_abort_message(const char*);
extern char __sF[];   // bionic_shims.c: char __sF[3*512]

static void wlog(const char* s){
  const char* lp=getenv("WWISE_LOG"); if(!lp||!*lp) lp="/tmp/sor4-wwise.log";
  FILE* f=fopen(lp,"a"); if(f){fprintf(f,"%s\n",s); fclose(f);}
}
// Log VERBOSO por-evento de audio (set_switch/set_state): no gameplay roda
// milhares de vezes -> ~15k linhas/sessao + fopen por chamada. Gateado por
// SOR4_NATLOG=1 (default off); os markers de init seguem sempre via wlog().
static int wlog_verbose(void){
  static int v=-1;
  if(v<0){ const char* e=getenv("SOR4_NATLOG"); v=(e&&e[0]=='1')?1:0; }
  return v;
}

// ---------------- __sF (bionic stdio std streams) ----------------
// Wwise faz &__sF[idx]; nossos wrappers detectam o range e mandam p/ glibc.
#define SF_STRIDE 512
static FILE* sf_map(void* p){
  if((char*)p >= __sF && (char*)p < __sF+3*SF_STRIDE){
    long idx=((char*)p-__sF)/SF_STRIDE;
    return idx==0?stdin: idx==1?stdout: stderr;
  }
  return (FILE*)p;
}
static int w_fprintf(void* st,const char* fmt,...){ va_list a; va_start(a,fmt); int r=vfprintf(sf_map(st),fmt,a); va_end(a); return r; }
static int w_vfprintf(void* st,const char* fmt,va_list a){ return vfprintf(sf_map(st),fmt,a); }
static int w_fputc(int c,void* st){ return fputc(c,sf_map(st)); }
static int w_fputs(const char* s,void* st){ return fputs(s,sf_map(st)); }
static size_t w_fwrite(const void* p,size_t s,size_t n,void* st){ return fwrite(p,s,n,sf_map(st)); }
static int w_fflush(void* st){ return fflush(st?sf_map(st):NULL); }
static int w_feof(void* st){ return feof(sf_map(st)); }
static int w_fileno(void* st){ return fileno(sf_map(st)); }

// ---------------- liblog ----------------
static void w_openlog(const char* a,int b,int c){(void)a;(void)b;(void)c;}
static void w_closelog(void){}
// Wwise loga seus erros/monitor via syslog (build importa openlog/syslog/closelog).
// Roteamos p/ o wwise.log p/ ENXERGAR diagnosticos internos (codec/decode/init).
static void w_syslog(int pri,const char* fmt,...){
  (void)pri; char msg[1024]; va_list a; va_start(a,fmt); vsnprintf(msg,sizeof(msg),fmt,a); va_end(a);
  char b[1100]; snprintf(b,sizeof(b),"[WWISE-SYSLOG] %s",msg); wlog(b);
}

// ---------------- AAssetManager (carrega os bancos de g_bankbase) ----------------
static char g_bankbase[512]="../gameassets/";
typedef struct { FILE* f; long len; int is_music; char path[1024]; } AAsset;

// --- lista de wems que sao MUSICA (music_ids.txt, gerado pelo parser via HIRC) ---
// Roteamos a MUSICA por esta lista (precisa, 0 overlap c/ SFX) em vez de por TAMANHO, que
// classificava ~300 segmentos de musica <1.5MB como SFX streamed PARALELO -> uma musica
// por cima da outra dentro da fase. Com a lista, todo segmento de musica vai p/ a fonte
// UNICA (g_mus_src), que SUBSTITUI o anterior -> sem sobreposicao. Custo: ~7KB de RAM.
static uint32_t* g_music_ids=NULL; static int g_nmusic=0;
static int cmp_u32(const void* a,const void* b){ uint32_t x=*(const uint32_t*)a,y=*(const uint32_t*)b; return (x>y)-(x<y); }
static void load_music_ids(void){
  const char* dir=getenv("SOR4_AUDIO"); if(!dir||!*dir) return;
  char path[1100]; snprintf(path,sizeof(path),"%s/music_ids.txt",dir);
  FILE* f=fopen(path,"r"); if(!f){ wlog("[wwise] music_ids.txt ausente -> roteamento por tamanho"); return; }
  int cap=512; g_music_ids=malloc(cap*sizeof(uint32_t)); g_nmusic=0;
  char line[64];
  while(g_music_ids && fgets(line,sizeof(line),f)){
    uint32_t v=(uint32_t)strtoul(line,NULL,10);
    if(v){ if(g_nmusic>=cap){ cap*=2; uint32_t* np=realloc(g_music_ids,cap*sizeof(uint32_t)); if(!np)break; g_music_ids=np; } g_music_ids[g_nmusic++]=v; }
  }
  fclose(f);
  if(g_music_ids) qsort(g_music_ids,g_nmusic,sizeof(uint32_t),cmp_u32);
  char b[96]; snprintf(b,sizeof(b),"[wwise] music_ids carregados: %d",g_nmusic); wlog(b);
}
static int is_music_id(uint32_t id){      // 1=musica, 0=nao, -1=sem lista (usa fallback tamanho)
  if(g_nmusic<=0) return -1;
  int lo=0,hi=g_nmusic-1;
  while(lo<=hi){ int m=(lo+hi)/2; if(g_music_ids[m]==id)return 1; if(g_music_ids[m]<id)lo=m+1; else hi=m-1; }
  return 0;
}
static uint32_t wem_id_from_path(const char* p){
  const char* b=strrchr(p,'/'); b=b?b+1:p; return (uint32_t)strtoul(b,NULL,10);
}

static void* w_AAssetManager_fromJava(void* env,void* obj){ (void)env;(void)obj; wlog("[wwise] AAssetManager_fromJava"); return (void*)0x1; }
static void* w_AAssetManager_open(void* mgr,const char* fn,int mode){
  (void)mgr;(void)mode;
  char path[1024];
  if(fn && fn[0]=='/') snprintf(path,sizeof(path),"%s",fn);           // ja absoluto (base+bank)
  else snprintf(path,sizeof(path),"%s%s",g_bankbase,fn?fn:"");        // relativo -> prefixa base
  FILE* f=fopen(path,"rb"); if(!f){ char b[1100]; snprintf(b,sizeof(b),"[wwise] AAsset open FALHOU %s",path); wlog(b); return NULL; }
  fseek(f,0,SEEK_END); long len=ftell(f); fseek(f,0,SEEK_SET);
  AAsset* a=malloc(sizeof(AAsset)); a->f=f; a->len=len; a->is_music=0; a->path[0]=0;
  char b[1100]; snprintf(b,sizeof(b),"[wwise] AAsset open %s (%ld)",path,len); wlog(b);
  // HIBRIDO: a Wwise nativa decide qual MUSICA/efeito streamed tocar e abre o .wem certo.
  // Distinguimos por TAMANHO: .wem GRANDE = MUSICA de fundo (loop, echo p/ ao_music_request,
  // para no fluxo original via close); .wem PEQUENO = EFEITO streamed one-shot (hit/voz/
  // stinger) -> toca uma vez SEM mexer na musica (antes qualquer .wem sequestrava a musica).
  { size_t pl=strlen(path);
    if(pl>4 && strcmp(path+pl-4,".wem")==0){
      // MUSICA pela lista do HIRC (music_ids.txt); sem lista -> fallback por TAMANHO.
      uint32_t wid=wem_id_from_path(path); int mus=is_music_id(wid);
      if(mus<0){ long minsz=1572864; const char* e=getenv("SOR4_MUSIC_MINSIZE"); if(e&&*e) minsz=atol(e); mus=(len>=minsz); }
      if(mus){
        if(ao_music_request(path)){
          a->is_music=1;
          strncpy(a->path,path,sizeof(a->path)-1);
          a->path[sizeof(a->path)-1]=0;
        }
      } else ao_play_streamed_sfx(path);
    } }
  return a;
}
static unsigned long g_rd_calls=0, g_rd_bytes=0;
static int w_AAsset_read(void* as,void* buf,size_t cnt){ AAsset* a=as; if(!a||!a->f) return -1; int r=(int)fread(buf,1,cnt,a->f);
  if(getenv("WWISE_RDLOG")){ g_rd_calls++; if(r>0)g_rd_bytes+=r; if(g_rd_calls<=3||g_rd_calls%200==0){ char b[160]; snprintf(b,sizeof(b),"[wwise] AAsset_read #%lu cnt=%zu got=%d totbytes=%lu",g_rd_calls,cnt,r,g_rd_bytes); wlog(b);} }
  return r; }
static long w_AAsset_seek(void* as,long off,int whence){ AAsset* a=as; if(!a||!a->f) return -1; fseek(a->f,off,whence); return ftell(a->f); }
static long w_AAsset_getLength(void* as){ AAsset* a=as; return a?a->len:0; }
static void w_AAsset_close(void* as){ AAsset* a=as; if(a){ if(a->is_music) ao_music_close(a->path); if(a->f)fclose(a->f); free(a);} }
static void* w_AAssetManager_openDir(void* m,const char* d){ (void)m;(void)d; return NULL; }
static void w_AAssetDir_close(void* d){ (void)d; }

// ---------------- tabela extra (45 simbolos que faltavam) ----------------
static DynLibFunction extra[] = {
  // libm glibc
  {"acosf",(uintptr_t)&acosf},{"asinf",(uintptr_t)&asinf},{"exp2f",(uintptr_t)&exp2f},
  {"log10f",(uintptr_t)&log10f},{"sincos",(uintptr_t)&sincos},{"sincosf",(uintptr_t)&sincosf},
  // libc glibc
  {"feof",(uintptr_t)&w_feof},{"flockfile",(uintptr_t)&flockfile},{"freopen",(uintptr_t)&freopen},
  {"fseeko",(uintptr_t)&fseeko},{"ftello",(uintptr_t)&ftello},{"funlockfile",(uintptr_t)&funlockfile},
  {"getauxval",(uintptr_t)&getauxval},{"syscall",(uintptr_t)&syscall},{"vasprintf",(uintptr_t)&vasprintf},
  {"vfprintf",(uintptr_t)&w_vfprintf},{"dlopen",(uintptr_t)&dlopen},{"dlsym",(uintptr_t)&dlsym},
  {"dlclose",(uintptr_t)&dlclose},{"dlerror",(uintptr_t)&dlerror},{"dl_iterate_phdr",(uintptr_t)&dl_iterate_phdr},
  {"sched_get_priority_max",(uintptr_t)&sched_get_priority_max},{"sched_get_priority_min",(uintptr_t)&sched_get_priority_min},
  // stdio wrappers (p/ __sF)
  {"fprintf",(uintptr_t)&w_fprintf},{"fputc",(uintptr_t)&w_fputc},{"fputs",(uintptr_t)&w_fputs},
  {"fwrite",(uintptr_t)&w_fwrite},{"fflush",(uintptr_t)&w_fflush},
  {"__sF",(uintptr_t)__sF},
  // liblog
  {"openlog",(uintptr_t)&w_openlog},{"closelog",(uintptr_t)&w_closelog},{"syslog",(uintptr_t)&w_syslog},
  // bionic _chk + props
  {"__memcpy_chk",(uintptr_t)&__memcpy_chk},{"__memmove_chk",(uintptr_t)&__memmove_chk},
  {"__strlen_chk",(uintptr_t)&__strlen_chk},{"__strncat_chk",(uintptr_t)&__strncat_chk},
  {"__strncpy_chk",(uintptr_t)&__strncpy_chk},{"__strncpy_chk2",(uintptr_t)&__strncpy_chk2},
  {"__vsnprintf_chk",(uintptr_t)&__vsnprintf_chk},
  {"__system_property_get",(uintptr_t)&__system_property_get},
  {"android_set_abort_message",(uintptr_t)&android_set_abort_message},
  // AAssetManager (bancos)
  {"AAssetManager_fromJava",(uintptr_t)&w_AAssetManager_fromJava},
  {"AAssetManager_open",(uintptr_t)&w_AAssetManager_open},
  {"AAsset_read",(uintptr_t)&w_AAsset_read},{"AAsset_seek",(uintptr_t)&w_AAsset_seek},
  {"AAsset_getLength",(uintptr_t)&w_AAsset_getLength},{"AAsset_close",(uintptr_t)&w_AAsset_close},
  {"AAssetManager_openDir",(uintptr_t)&w_AAssetManager_openDir},{"AAssetDir_close",(uintptr_t)&w_AAssetDir_close},
};

// ---------------- carga da Wwise real ----------------
#define WWISE_REAL "./libs/libWwise.real.so"
#define HEAP_MB 64
static int g_loaded=0;
static int g_init_ok=0;   // so encaminha aos trampolins se a init REAL deu certo (senao crasha)
typedef int  (*fn_init_t)(const char*);
typedef void (*fn_void_t)(void);

/* Crash-handler diagnostico TARDIO (opt-in WWISE_CRASHLOG=1): instalado DEPOIS da init do
 * Wwise/.NET p/ capturar o backtrace NATIVO do SIGSEGV (em stderr->log.txt) antes de morrer.
 * async-safe (so write/backtrace_symbols_fd), alt-stack proprio. So p/ diagnostico. */
static volatile int g_in_crash = 0;
static void wwise_crash_handler(int sig, siginfo_t* info, void* uc){
  if(g_in_crash) _exit(128+sig); g_in_crash=1;
  ucontext_t* u=(ucontext_t*)uc; uintptr_t pcv=0;
#if defined(__aarch64__)
  if(u) pcv=(uintptr_t)u->uc_mcontext.pc;
#endif
  char hdr[160]; int n=snprintf(hdr,sizeof(hdr),"\n[WWISE-CRASH] sig=%d addr=%p pc=%p\n",
    sig, info?info->si_addr:(void*)0, (void*)pcv);
  (void)!write(2,hdr,n);
  void* bt[64]; int nb=backtrace(bt,64);
  backtrace_symbols_fd(bt,nb,2);
  _exit(128+sig);
}
static void install_wwise_crash_handler(void){
  if(!getenv("WWISE_CRASHLOG")) return;
  static char altstk[262144];
  stack_t ss; ss.ss_sp=altstk; ss.ss_size=sizeof altstk; ss.ss_flags=0; sigaltstack(&ss,NULL);
  struct sigaction sa; memset(&sa,0,sizeof sa);
  sa.sa_sigaction=wwise_crash_handler; sa.sa_flags=SA_SIGINFO|SA_ONSTACK; sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV,&sa,NULL); sigaction(SIGBUS,&sa,NULL); sigaction(SIGABRT,&sa,NULL);
  wlog("[wwise-native] crash-handler diagnostico instalado (WWISE_CRASHLOG=1)");
}

static void load_real(void){
  if(g_loaded) return;
  const char* path=getenv("WWISE_REAL"); if(!path||!*path) path=WWISE_REAL;
  wlog("[wwise-native] carregando libWwise REAL...");
  size_t heap_size=(size_t)HEAP_MB*1024*1024;
  void* heap=mmap(NULL,heap_size,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  if(heap==MAP_FAILED){ wlog("[wwise-native] mmap heap FALHOU"); return; }
  if(so_load(path,heap,heap_size)<0){ wlog("[wwise-native] so_load FALHOU"); return; }
  if(so_relocate()<0){ wlog("[wwise-native] so_relocate FALHOU"); return; }
  // tabela combinada: extra primeiro (override) + dynlib_functions
  int ntot=(int)dynlib_numfunctions + (int)(sizeof(extra)/sizeof(extra[0]));
  DynLibFunction* comb=malloc(ntot*sizeof(DynLibFunction));
  int n=0;
  for(unsigned i=0;i<sizeof(extra)/sizeof(extra[0]);i++) comb[n++]=extra[i];
  for(size_t i=0;i<dynlib_numfunctions;i++) comb[n++]=dynlib_functions[i];
  if(so_resolve(comb,n,1)<0){ wlog("[wwise-native] so_resolve FALHOU"); free(comb); return; }
  free(comb);
  // Os 3 checks de PATH (AddBasePath/2o-check/SetBasePath) FALHAM no ambiente Android-fake
  // (validam path asset-relativo) mas NAO sao necessarios: nosso AAssetManager_open le os
  // bancos por path absoluto. NOPamos os b.ne deles p/ a init suceder. (offsets da v1.4.5)
  { const char* nz=getenv("WWISE_NOP");
    char defnop[]="0x12242c,0x122438,0x122510";
    if(!nz||!*nz) nz=defnop;
    if(nz&&*nz){ so_make_text_writable();
      char buf[256]; strncpy(buf,nz,sizeof(buf)-1); buf[sizeof(buf)-1]=0;
      char* p=buf; while(*p){ unsigned long off=strtoul(p,NULL,0);
        if(off){ *(uint32_t*)((char*)text_base+off)=0xd503201fu; char b[80]; snprintf(b,sizeof(b),"[wwise-native] NOP @0x%lx",off); wlog(b); }
        char* c=strchr(p,','); if(!c)break; p=c+1; }
      so_flush_caches();
    } }
  so_flush_caches();
  so_execute_init_array();
  g_loaded=1;
  { char b[160]; uintptr_t ni=so_find_addr("native_wwise_init");
    snprintf(b,sizeof(b),"[wwise-native] text_base=%p native_wwise_init=0x%lx (off 0x122350)",text_base,(unsigned long)ni); wlog(b); }
  wlog("[wwise-native] libWwise REAL carregada OK");
  // Wwise precisa de uma JavaVM (capturada via JNI_OnLoad pelo runtime Android, que nao temos).
  // Damos a JavaVM/JNIEnv FAKE do jni_shim e chamamos JNI_OnLoad + native_android_preinit nos.
  jni_shim_init(&g_fake_vm,&g_fake_env);
  uintptr_t jol=so_find_addr("JNI_OnLoad");
  if(jol){ int v=((int(*)(void*,void*))jol)(g_fake_vm,NULL); char b[64]; snprintf(b,sizeof(b),"[wwise-native] JNI_OnLoad=%d",v); wlog(b); }
  uintptr_t pre=so_find_addr("native_android_preinit");
  if(pre){ ((long(*)(void*))pre)((void*)0x1000); wlog("[wwise-native] native_android_preinit(fake activity) chamado"); }
  install_wwise_crash_handler();   /* TARDIO: depois do Wwise init (opt-in WWISE_CRASHLOG=1) */
  { const char* gp=getenv("WWISE_GDB"); if(gp&&*gp){ int s=atoi(gp); if(s<=0)s=40; so_make_text_writable(); char b[120]; snprintf(b,sizeof(b),"[wwise-native] PAUSANDO %ds p/ gdb (pid=%d text_base=%p)...",s,(int)getpid(),text_base); wlog(b); sleep(s); wlog("[wwise-native] resumindo"); } }
}

__attribute__((constructor)) static void ctor(void){ load_real(); }

#define CALL0(name,ret) ret name(void){ if(!g_loaded) load_real(); uintptr_t a=so_find_addr(#name); if(!a) return (ret)0; return ((ret(*)(void))a)(); }
#define TRAMP(name) uintptr_t __tr_##name(void){ if(!g_loaded) load_real(); return so_find_addr(#name); }

// ----- trampolins dos 21 native_wwise_* (+ preinit) -----
long native_android_preinit(void* activity){ if(!g_loaded) load_real(); char b[80]; snprintf(b,sizeof(b),"[wwise-native] preinit activity=%p",activity); wlog(b); uintptr_t a=so_find_addr("native_android_preinit"); return a?((long(*)(void*))a)(activity):0; }

static pthread_mutex_t g_render_mtx = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_pump_run = 0;
static pthread_t g_pump_thread;
static int g_pump_started = 0;
static void render_audio_locked(void){
  uintptr_t a=so_find_addr("native_wwise_update"); if(!a) return;
  pthread_mutex_lock(&g_render_mtx);
  ((void(*)(void))a)();                 // = AK::SoundEngine::RenderAudio(true)
  pthread_mutex_unlock(&g_render_mtx);
}
// AUDIO THREAD UNICA acoplada: RenderAudio() (behavioral, prepara o frame) -> pump
// (dispara o callback do BufferQueue, onde a Wwise RENDERIZA o frame com params frescos).
// Acoplar elimina o desync entre behavioral e render que deixava o mix VAZIO.
static void* pump_thread_fn(void* a){ (void)a;
  { char b[64]; snprintf(b,sizeof(b),"[wwise-native] audio thread tid=%ld",syscall(SYS_gettid)); wlog(b);}
  // A engine nativa serve SO p/ SELECIONAR a musica (RenderAudio processa eventos/states
  // -> abre o .wem certo por contexto, que nos echoamos p/ o OpenAL). O AUDIO dela e'
  // descartado. Por padrao NAO bombeamos o sink (WWISE_NOPUMP): assim a Wwise nao
  // decodifica a musica em tempo real (decode que meu OpenAL ja faz) -> alivia MUITO a
  // CPU no combate pesado. RenderAudio continua p/ a logica de selecao de musica.
  unsigned us=20000; { const char* e=getenv("WWISE_TICK_US"); if(e&&*e) us=(unsigned)atoi(e); }
  // pump LIGADO por padrao: a Wwise nativa precisa "tocar" a musica em tempo real p/ as
  // TRANSICOES de musica interativa dispararem (loading->fase). Sem isso a fase ficava
  // com a musica do loading. WWISE_NOPUMP=1 desliga (mais leve, mas quebra transicoes).
  int pump = getenv("WWISE_NOPUMP")==NULL;
  wlog(pump?"[wwise-native] pump LIGADO (transicoes de musica funcionam)":"[wwise-native] pump DESLIGADO (leve, sem transicoes)");
  while(g_pump_run){ render_audio_locked(); if(pump) opensles_shim_pump_callbacks(); usleep(us); }
  return NULL;
}
static void start_pump_thread(void){
  if(g_pump_started) return;
  g_pump_run=1;
  if(pthread_create(&g_pump_thread,NULL,pump_thread_fn,NULL)==0){
    g_pump_started=1;
    wlog("[wwise-native] audio thread iniciada (RenderAudio+pump acoplados)");
  } else {
    g_pump_run=0;
    wlog("[wwise-native] audio thread FALHOU");
  }
}
static void stop_pump_thread(void){
  if(!g_pump_started) return;
  g_pump_run=0;
  pthread_join(g_pump_thread,NULL);
  g_pump_started=0;
}

int native_wwise_init(const char* p){
  if(!g_loaded) load_real();
  // O jogo passa get_data_folder() VAZIO no nosso port -> AddBasePath falha.
  // Substituimos pelo dir dos bancos (gameassets, com barra final).
  const char* base=getenv("SOR4_BANKDIR"); if(!base||!*base) base="../gameassets/";
  if(p&&*p) base=p;
  strncpy(g_bankbase,base,sizeof(g_bankbase)-1);
  char b[600]; snprintf(b,sizeof(b),"[wwise-native] init path='%s'",base); wlog(b);
  uintptr_t a=so_find_addr("native_wwise_init"); if(!a){ wlog("[wwise-native] init addr=0"); return 0; }
  int r=((int(*)(const char*))a)(base);
  g_init_ok = (r!=0);
  if(g_init_ok){ start_pump_thread(); ao_init(); load_music_ids(); wlog("[wwise-native] audioout custom pronto; RTPCs de volume preservados"); }
  // diagnostico: le os globais que o 2o check (0x256b24) testa
  { unsigned char* fb=(unsigned char*)((char*)text_base + 0x2d0628);
    void** pl=(void**)((char*)text_base + 0x2d1dc0);
    char b3[160]; snprintf(b3,sizeof(b3),"[wwise-native] DIAG flag@2d0628=%d plist@2d1dc0=%p",(int)*fb,*pl); wlog(b3); }
  char b2[64]; snprintf(b2,sizeof(b2),"[wwise-native] real init=%d",r); wlog(b2);
  return r;
}
#define FWD_V(name) void name(void){ if(!g_init_ok) return; uintptr_t a=so_find_addr(#name); if(a)((void(*)(void))a)(); }
#define FWD_I0(name) int name(void){ if(!g_init_ok) return 0; uintptr_t a=so_find_addr(#name); return a?((int(*)(void))a)():0; }
#define FWD_S(name) int name(const char* s){ if(!g_init_ok) return 0; uintptr_t a=so_find_addr(#name); return a?((int(*)(const char*))a)(s):0; }

void native_wwise_destroy(void){
  // Primeiro para o nosso RenderAudio concorrente; so depois encerra a Wwise real.
  // O audio OpenAL custom fica vivo durante o destroy real porque AAsset_close ainda
  // pode notificar o fim de um segmento. Tudo e idempotente para o destructor fallback.
  stop_pump_thread();
  if(g_init_ok){
    g_init_ok=0;
    uintptr_t a=so_find_addr("native_wwise_destroy");
    if(a) ((void(*)(void))a)();
  }
  ao_shutdown();
  wlog("[wwise-native] destroy OK");
}
FWD_S(native_wwise_loadbank)
FWD_S(native_wwise_unloadbank)
int native_wwise_post_event(const char* e){ if(!g_init_ok)return 0; ao_post_event(e); uintptr_t a=so_find_addr("native_wwise_post_event"); return a?((int(*)(const char*))a)(e):0; }
FWD_S(native_wwise_post_trigger)
int native_wwise_post_event_with_id(const char* e,uint64_t o){ if(!g_init_ok)return 0; ao_post_event(e); uintptr_t a=so_find_addr("native_wwise_post_event_with_id"); return a?((int(*)(const char*,uint64_t))a)(e,o):0; }
int native_wwise_post_trigger_with_id(const char* t,uint64_t o){ if(!g_init_ok)return 0; uintptr_t a=so_find_addr("native_wwise_post_trigger_with_id"); return a?((int(*)(const char*,uint64_t))a)(t,o):0; }
void native_wwise_register_gameobject_with_id(uint64_t id){ if(!g_init_ok)return; uintptr_t a=so_find_addr("native_wwise_register_gameobject_with_id"); if(a)((void(*)(uint64_t))a)(id); }
void native_wwise_register_gameobject_with_id_and_name(uint64_t id,const char* n){ if(!g_init_ok)return; uintptr_t a=so_find_addr("native_wwise_register_gameobject_with_id_and_name"); if(a)((void(*)(uint64_t,const char*))a)(id,n); }
void native_wwise_set_switch(const char* g,const char* s,uint64_t o){ if(!g_init_ok)return; if(wlog_verbose()){ char b[200]; snprintf(b,sizeof(b),"[wwise-native] set_switch grp='%s' sw='%s' obj=%llu",g?g:"",s?s:"",(unsigned long long)o); wlog(b);} uintptr_t a=so_find_addr("native_wwise_set_switch"); if(a)((void(*)(const char*,const char*,uint64_t))a)(g,s,o); }
void native_wwise_set_state(const char* g,const char* s){ if(!g_init_ok)return; if(wlog_verbose()){ char b[200]; snprintf(b,sizeof(b),"[wwise-native] set_state grp='%s' state='%s'",g?g:"",s?s:""); wlog(b);} uintptr_t a=so_find_addr("native_wwise_set_state"); if(a)((void(*)(const char*,const char*))a)(g,s); }
int native_wwise_set_listener_position(void* v){ if(!g_init_ok)return 0; uintptr_t a=so_find_addr("native_wwise_set_listener_position"); return a?((int(*)(void*))a)(v):0; }
int native_wwise_set_gameobject_position(uint64_t o,void* v){ if(!g_init_ok)return 0; uintptr_t a=so_find_addr("native_wwise_set_gameobject_position"); return a?((int(*)(uint64_t,void*))a)(o,v):0; }
// O jogo envia MusicVolume/SfxVolume normalizados a cada frame. A Wwise real continua
// recebendo o valor original para preservar sua logica, e o mixer OpenAL custom recebe
// o mesmo RTPC porque e ele quem produz o audio audivel neste port.
static int real_set_rtpc(const char* n,float v){ uintptr_t a=so_find_addr("native_wwise_set_rtpc_value"); return a?((int(*)(const char*,float))a)(n,v):0; }

void native_wwise_update(void){
  if(!g_init_ok) return;
  static unsigned uc=0;
  render_audio_locked();                     // serializado com o render driver
  if(uc==0||uc==600||uc==3000){ char b[80]; snprintf(b,sizeof(b),"[wwise-native] update() chamada #%u (RenderAudio do JOGO)",uc); wlog(b);} uc++;
}

int native_wwise_set_rtpc_value(const char* n,float v){
  if(!g_init_ok)return 0;
  ao_set_rtpc_volume(n,v);
  return real_set_rtpc(n,v);
}
int native_wwise_set_rtpc_value_with_id(const char* n,float v,uint64_t o){ if(!g_init_ok)return 0; uintptr_t a=so_find_addr("native_wwise_set_rtpc_value_with_id"); return a?((int(*)(const char*,float,uint64_t))a)(n,v,o):0; }
int native_wwise_get_music_event(void* ev){ if(!g_init_ok)return 0; uintptr_t a=so_find_addr("native_wwise_get_music_event"); return a?((int(*)(void*))a)(ev):0; }
int native_wwise_get_total_memory(void){ if(!g_init_ok)return 0; uintptr_t a=so_find_addr("native_wwise_get_total_memory"); return a?((int(*)(void))a)():0; }
int native_wwise_is_game_object_active(uint64_t o){ if(!g_init_ok)return 0; uintptr_t a=so_find_addr("native_wwise_is_game_object_active"); return a?((int(*)(uint64_t))a)(o):0; }
void native_wwise_unregister_inactive_game_objects(void){ if(!g_init_ok)return; uintptr_t a=so_find_addr("native_wwise_unregister_inactive_game_objects"); if(a)((void(*)(void))a)(); }
long get_total_memory_stub(void){ return 0; }
long sor4_gl_noop(void){ return 0; }

// Environment.Exit e alguns caminhos do jogo nao chamam o destroy gerenciado. O
// fallback roda antes do unload das bibliotecas abertas por este wrapper e evita
// que o destructor do OpenAL encontre um device/context ainda registrado.
__attribute__((destructor)) static void wwise_wrapper_fini(void){
  native_wwise_destroy();
}
