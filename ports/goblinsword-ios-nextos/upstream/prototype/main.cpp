#include "runtime.h"
#include "macho_loader.h"
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <ucontext.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>

extern void rt_services_boot();
static MachImage image;
static uintptr_t stack_low=0,stack_high=0;
static void signal_handler(int sig,siginfo_t*info,void*context){
    rt_idle_shutdown();
    auto u=static_cast<ucontext_t*>(context);
    fprintf(stderr,"SIGNAL %d fault=%p guest_base=%p slide=%#lx\n",sig,info->si_addr,image.base,(unsigned long)image.slide);
#if defined(__aarch64__)
    fprintf(stderr,"PC=%#llx LR=%#llx SP=%#llx guest_pc=%#llx\n",
        (unsigned long long)u->uc_mcontext.pc,(unsigned long long)u->uc_mcontext.regs[30],
        (unsigned long long)u->uc_mcontext.sp,(unsigned long long)(u->uc_mcontext.pc-image.slide));
    for(int i=0;i<8;i++)fprintf(stderr,"x%d=%#llx ",i,(unsigned long long)u->uc_mcontext.regs[i]);
    fprintf(stderr,"\n");
    uintptr_t fp=u->uc_mcontext.regs[29];
    for(unsigned depth=0;depth<32 && fp>=stack_low && fp+16<=stack_high && !(fp&15);++depth){
        auto words=reinterpret_cast<const uintptr_t*>(fp);
        fprintf(stderr,"FRAME %u fp=%#lx lr=%#lx guest_lr=%#lx\n",depth,(unsigned long)fp,(unsigned long)words[1],(unsigned long)(words[1]-image.slide));
        if(words[0]<=fp)break;
        fp=words[0];
    }
#endif
    fflush(stderr);_exit(128+sig);
}
int main(int argc,char**argv){
    setbuf(stdout,nullptr);setbuf(stderr,nullptr);
    pthread_attr_t thread_attributes;
    if(!pthread_getattr_np(pthread_self(),&thread_attributes)){
        void* address=nullptr;size_t size=0;
        if(!pthread_attr_getstack(&thread_attributes,&address,&size)){stack_low=reinterpret_cast<uintptr_t>(address);stack_high=stack_low+size;}
        pthread_attr_destroy(&thread_attributes);
    }
    fprintf(stderr,"Goblin Sword iOS prototype: native ARM64 Mach-O, diagnostic only\n");
    Dl_info host_info{};
    if(dladdr(reinterpret_cast<void*>(&main),&host_info))
        fprintf(stderr,"HOST_IMAGE base=%p\n",host_info.dli_fbase);
    if(argc!=3){fprintf(stderr,"usage: %s executable.macho app-data-directory\n",argv[0]);return 2;}
    rt_data_root=argv[2];
    struct sigaction sa{};sa.sa_sigaction=signal_handler;sa.sa_flags=SA_SIGINFO;
    sigemptyset(&sa.sa_mask);for(int s:{SIGSEGV,SIGBUS,SIGILL,SIGABRT,SIGALRM,SIGTERM,SIGINT,SIGHUP})sigaction(s,&sa,nullptr);
    const char* seconds=getenv("GOBLIN_DIAGNOSTIC_SECONDS");alarm(seconds?strtoul(seconds,nullptr,10):30);
    // Register host classes/methods before binding classrefs and constant strings.
    rt_install_base();rt_install_foundation();rt_install_graphics();rt_install_services();rt_install_audio();rt_install_idle();
    std::string error;
    if(!macho_load(argv[1],rt_resolve,nullptr,image,error)){fprintf(stderr,"LOAD_FAILED %s\n",error.c_str());return 3;}
    fprintf(stderr,"LOADED base=%p slide=%#lx entry=%p rebases=%zu binds=%zu\n",image.base,(unsigned long)image.slide,image.entry,image.rebase_count,image.bind_count);
    rt_register_image(image);
    rt_call_load_methods();
    for(void*f:image.mod_init_functions)reinterpret_cast<void(*)()>(f)();
    rt_services_boot();
    fprintf(stderr,"ENTER_NATIVE_MAIN\n");
    char guest_name[]="Goblin Sword";char*guest_argv[]={guest_name,nullptr};
    int result=reinterpret_cast<int(*)(int,char**)>(image.entry)(1,guest_argv);
    fprintf(stderr,"NATIVE_MAIN_RETURN %d\n",result);return result;
}
