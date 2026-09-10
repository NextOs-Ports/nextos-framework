#include "runtime.h"
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

// Linux VT setterm CSI 9;<minutes>] updates the same blankinterval exported as
// /sys/module/kernel/parameters/consoleblank and calls poke_blanked_console().
// TIOCL_UNBLANKSCREEN alone does not renew a timer on an already lit console.
// No KDSETMODE, framebuffer blanking, input injection, or saved pixels are used.
// The main signal handler must call rt_idle_shutdown() before its _exit().
namespace {
constexpr const char* timer_path="/sys/module/kernel/parameters/consoleblank";
volatile sig_atomic_t tty_fd=-1;
volatile sig_atomic_t original_seconds=-1;
volatile sig_atomic_t original_mode=-1;
char restore_command[32]{};
volatile sig_atomic_t restore_length=0;
volatile sig_atomic_t restore_pending=0;
volatile sig_atomic_t requested_disabled=0;
bool installed=false;
timespec last_check{};

// System-call-only helpers are also used by signal cleanup.
bool write_all(int fd,const char* text,size_t size){
    while(size){ssize_t n=write(fd,text,size);if(n<0&&errno==EINTR)continue;if(n<=0)return false;text+=n;size-=size_t(n);}return true;
}
int timer_seconds(){
    int fd=open(timer_path,O_RDONLY|O_CLOEXEC);if(fd<0)return -1;
    char text[32];ssize_t n;
    do{n=read(fd,text,sizeof(text));}while(n<0&&errno==EINTR);
    close(fd);if(n<=0)return -1;
    int value=0;bool digit=false;
    for(ssize_t i=0;i<n;i++){
        if(text[i]>='0'&&text[i]<='9'){
            digit=true;if(value>360000)return -1;value=value*10+text[i]-'0';
        }else if(text[i]=='\n'||text[i]=='\r'||text[i]==' '||text[i]=='\t'){
            for(ssize_t j=i;j<n;j++)if(text[j]!='\n'&&text[j]!='\r'&&text[j]!=' '&&text[j]!='\t')return -1;
            break;
        }else return -1;
    }
    return digit?value:-1;
}
void cleanup_warning(){
    static constexpr char message[]="[idle] RESTORE_FAILED: consoleblank did not return to its saved value; inspect kernel parameter before another launch\n";
    (void)write(STDERR_FILENO,message,sizeof(message)-1);
}
bool restore_timer(){
    if(!restore_pending)return true;
    // Preserve pending on failure so explicit shutdown can retry before exit.
    if(tty_fd<0||!write_all(tty_fd,restore_command,restore_length)||timer_seconds()!=original_seconds){cleanup_warning();return false;}
    restore_pending=0;return true;
}
void close_scope(){
    if(tty_fd>=0){close(tty_fd);tty_fd=-1;}
    original_seconds=-1;original_mode=-1;restore_length=0;
}
void acquire_scope(){
    int control=open("/dev/tty0",O_RDONLY|O_CLOEXEC|O_NOCTTY);
    if(control<0)rt_fail("idle inhibition cannot open real /dev/tty0");
    vt_stat state{};
    if(ioctl(control,VT_GETSTATE,&state)<0||state.v_active<1||state.v_active>63){close(control);rt_fail("idle inhibition cannot identify active VT");}
    close(control);
    char path[32];std::snprintf(path,sizeof(path),"/dev/tty%u",state.v_active);
    tty_fd=open(path,O_WRONLY|O_CLOEXEC|O_NOCTTY);
    if(tty_fd<0)rt_fail("idle inhibition cannot open active VT");
    int mode=-1;
    if(ioctl(tty_fd,KDGETMODE,&mode)<0){close_scope();rt_fail("idle inhibition cannot read real console mode");}
    original_mode=mode;
    original_seconds=timer_seconds();
    if(original_seconds<0){close_scope();rt_fail("idle inhibition cannot read consoleblank timer");}
    std::fprintf(stderr,"[idle] active=%s KDGETMODE=%d consoleblank=%d seconds\n",path,original_mode,original_seconds);
}
void set_disabled(Obj,Sel,bool disabled){
    if(disabled==bool(requested_disabled))return;
    if(!disabled){
        if(!restore_timer())rt_fail("cannot restore original consoleblank timer");
        requested_disabled=0;std::fprintf(stderr,"[idle] UIApplication released inhibitor; original timer restored\n");close_scope();return;
    }
    acquire_scope();
    if(original_mode==KD_GRAPHICS){
        // The kernel VT blanking path already ignores a graphics-mode console.
        requested_disabled=1;std::fprintf(stderr,"[idle] verified existing KD_GRAPHICS; console mode and timer unchanged\n");
    }else if(original_mode==KD_TEXT){
        if(original_seconds!=0){
            // CSI has minute precision and a 60-minute cap. Never round a saved
            // value: that would make the promised restoration inaccurate.
            if(original_seconds>3600||original_seconds%60){close_scope();rt_fail("consoleblank value cannot be restored exactly through VT minute API");}
            int n=std::snprintf(restore_command,sizeof(restore_command),"\033[9;%d]",original_seconds/60);
            if(n<=0||size_t(n)>=sizeof(restore_command)){close_scope();rt_fail("idle restore sequence overflow");}
            restore_length=n;
            // Publish restoration state before mutating the global timer so a
            // signal during the write can still restore the original setting.
            restore_pending=1;
            static constexpr char disable_command[]="\033[9;0]";
            if(!write_all(tty_fd,disable_command,sizeof(disable_command)-1)||timer_seconds()!=0){
                (void)restore_timer();rt_fail("consoleblank disable did not verify as zero");
            }
        }
        requested_disabled=1;std::fprintf(stderr,"[idle] consoleblank verified zero; saved original=%d seconds\n",original_seconds);
    }else{close_scope();rt_fail("unknown console mode for idle inhibition");}
    clock_gettime(CLOCK_MONOTONIC,&last_check);
}
bool get_disabled(Obj,Sel){return requested_disabled!=0;}
}

void rt_idle_tick(){
    if(!requested_disabled)return;
    timespec now{};if(clock_gettime(CLOCK_MONOTONIC,&now)<0)rt_fail("idle inhibitor monotonic clock failed");
    if(now.tv_sec-last_check.tv_sec<1)return;
    last_check=now;
    int mode=-1;
    if(tty_fd<0||ioctl(tty_fd,KDGETMODE,&mode)<0)rt_fail("idle inhibitor lost its console descriptor");
    if(original_mode==KD_GRAPHICS){if(mode!=KD_GRAPHICS)rt_fail("existing graphics-mode idle protection changed");}
    else if(mode!=KD_TEXT||timer_seconds()!=0)rt_fail("verified consoleblank inhibition changed during execution");
}
void rt_idle_shutdown(){
    // Async-signal-safe: no dynamic allocation, locks, Objective-C calls or stdio.
    int saved_errno=errno;
    (void)restore_timer();requested_disabled=0;
    if(!restore_pending)close_scope();
    errno=saved_errno;
}
void rt_install_idle(){
    if(installed)return;
    installed=true;
    rt_method("UIApplication","setIdleTimerDisabled:",reinterpret_cast<void*>(set_disabled));
    rt_method("UIApplication","isIdleTimerDisabled",reinterpret_cast<void*>(get_disabled));
    rt_method("UIApplication","idleTimerDisabled",reinterpret_cast<void*>(get_disabled));
    if(std::atexit(rt_idle_shutdown)!=0)rt_fail("cannot register idle inhibitor cleanup");
}
