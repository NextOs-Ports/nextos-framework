// A mocked Linux VT boundary: this test never opens or writes a real console.
// Compile with the --wrap options recorded in evidence/idle-host-tests.json.
#include "../prototype/idle.cpp"
#include <cassert>
#include <cstdarg>
#include <stdexcept>
#include <string>

namespace fixture {
constexpr int timer_fd=101,control_fd=102,console_fd=103;
int timer=600,mode=KD_TEXT,writes=0,console_closes=0;
bool fail_disable=false,fail_restore=false,ignore_disable=false,fail_timer_read=false;
time_t seconds=100;
std::vector<std::string> commands;
void reset(int saved_timer=600,int saved_mode=KD_TEXT){
    assert(!restore_pending);
    if(tty_fd>=0)close_scope();
    requested_disabled=0;restore_pending=0;restore_length=0;
    timer=saved_timer;mode=saved_mode;writes=0;console_closes=0;commands.clear();
    fail_disable=fail_restore=ignore_disable=fail_timer_read=false;seconds=100;
}
template<class F> void rejected(F operation,const char* expected){
    try{operation();assert(!"operation unexpectedly succeeded");}
    catch(const std::runtime_error& error){assert(std::string(error.what()).find(expected)!=std::string::npos);}
}
}

[[noreturn]] void rt_fail(const char* message){throw std::runtime_error(message);}
void rt_method(const char*,const char*,void*,bool){}
extern "C" ssize_t __real_write(int,const void*,size_t);
extern "C" int __wrap_open(const char* path,int,...){
    if(!std::strcmp(path,timer_path))return fixture::timer_fd;
    if(!std::strcmp(path,"/dev/tty0"))return fixture::control_fd;
    if(!std::strcmp(path,"/dev/tty1"))return fixture::console_fd;
    assert(!"unexpected open; real filesystem access is forbidden");errno=ENOENT;return -1;
}
extern "C" ssize_t __wrap_read(int fd,void* buffer,size_t size){
    assert(fd==fixture::timer_fd);
    if(fixture::fail_timer_read){errno=EIO;return -1;}
    const std::string text=std::to_string(fixture::timer)+"\n";
    assert(size>=text.size());std::memcpy(buffer,text.data(),text.size());return text.size();
}
extern "C" int __wrap_close(int fd){
    assert(fd==fixture::timer_fd||fd==fixture::control_fd||fd==fixture::console_fd);
    if(fd==fixture::console_fd)fixture::console_closes++;
    return 0;
}
extern "C" ssize_t __wrap_write(int fd,const void* buffer,size_t size){
    if(fd==STDERR_FILENO||fd==STDOUT_FILENO)return __real_write(fd,buffer,size);
    assert(fd==fixture::console_fd);
    const std::string command(static_cast<const char*>(buffer),size);
    fixture::commands.push_back(command);fixture::writes++;
    assert(command.size()>=6&&command.compare(0,4,"\033[9;")==0&&command.back()==']');
    const int minutes=std::stoi(command.substr(4,command.size()-5));
    if((minutes==0&&fixture::fail_disable)||(minutes!=0&&fixture::fail_restore)){errno=EIO;return -1;}
    if(minutes!=0||!fixture::ignore_disable)fixture::timer=minutes*60;
    return size;
}
extern "C" int __wrap_ioctl(int fd,unsigned long request,...){
    va_list args;va_start(args,request);void* output=va_arg(args,void*);va_end(args);
    if(request==VT_GETSTATE){assert(fd==fixture::control_fd);static_cast<vt_stat*>(output)->v_active=1;return 0;}
    if(request==KDGETMODE){assert(fd==fixture::console_fd);*static_cast<int*>(output)=fixture::mode;return 0;}
    assert(!"unexpected ioctl; no console-mode mutation is allowed");errno=EINVAL;return -1;
}
extern "C" int __wrap_clock_gettime(clockid_t clock,timespec* output){
    assert(clock==CLOCK_MONOTONIC);output->tv_sec=fixture::seconds;output->tv_nsec=0;return 0;
}

int main(){
    using namespace fixture;
    reset();set_disabled(nullptr,nullptr,true);
    assert(timer==0&&get_disabled(nullptr,nullptr)&&writes==1&&restore_pending);
    set_disabled(nullptr,nullptr,true);assert(writes==1);
    seconds+=2;rt_idle_tick();assert(writes==1);
    set_disabled(nullptr,nullptr,false);
    assert(timer==600&&!get_disabled(nullptr,nullptr)&&writes==2&&!restore_pending&&tty_fd<0);
    assert(commands[0]=="\033[9;0]"&&commands[1]=="\033[9;10]");
    rt_idle_shutdown();assert(writes==2);

    reset();set_disabled(nullptr,nullptr,true);errno=E2BIG;rt_idle_shutdown();
    assert(errno==E2BIG&&timer==600&&!restore_pending&&tty_fd<0&&!requested_disabled);
    rt_idle_shutdown();assert(writes==2);

    reset(0);set_disabled(nullptr,nullptr,true);seconds+=2;rt_idle_tick();rt_idle_shutdown();
    assert(timer==0&&writes==0&&mode==KD_TEXT);

    reset(600,KD_GRAPHICS);set_disabled(nullptr,nullptr,true);seconds+=2;rt_idle_tick();rt_idle_shutdown();
    assert(timer==600&&writes==0&&mode==KD_GRAPHICS);

    reset(601);rejected([]{set_disabled(nullptr,nullptr,true);},"restored exactly");
    assert(timer==601&&writes==0&&tty_fd<0&&!requested_disabled);
    reset(3660);rejected([]{set_disabled(nullptr,nullptr,true);},"restored exactly");
    assert(timer==3660&&writes==0&&tty_fd<0&&!requested_disabled);

    reset();ignore_disable=true;rejected([]{set_disabled(nullptr,nullptr,true);},"did not verify");
    assert(timer==600&&writes==2&&!requested_disabled&&!restore_pending);rt_idle_shutdown();

    reset();fail_disable=true;rejected([]{set_disabled(nullptr,nullptr,true);},"did not verify");
    assert(timer==600&&writes==2&&!requested_disabled&&!restore_pending);rt_idle_shutdown();

    reset();set_disabled(nullptr,nullptr,true);fail_restore=true;
    rejected([]{set_disabled(nullptr,nullptr,false);},"cannot restore original");
    assert(timer==0&&restore_pending&&requested_disabled&&tty_fd>=0);
    fail_restore=false;rt_idle_shutdown();assert(timer==600&&!restore_pending&&tty_fd<0);

    reset();set_disabled(nullptr,nullptr,true);timer=120;seconds+=2;
    rejected([]{rt_idle_tick();},"inhibition changed");rt_idle_shutdown();assert(timer==600);

    reset(600,KD_GRAPHICS);set_disabled(nullptr,nullptr,true);mode=KD_TEXT;seconds+=2;
    rejected([]{rt_idle_tick();},"protection changed");rt_idle_shutdown();assert(timer==600&&writes==0&&mode==KD_TEXT);

    reset();fail_timer_read=true;rejected([]{set_disabled(nullptr,nullptr,true);},"cannot read consoleblank");
    assert(timer==600&&writes==0&&tty_fd<0&&!requested_disabled);

    reset(600,17);rejected([]{set_disabled(nullptr,nullptr,true);},"unknown console mode");
    assert(timer==600&&writes==0&&tty_fd<0&&!requested_disabled);
    std::puts("PASS: idle VT disable/restore, idempotence, shutdown errno, existing zero/graphics, exact restoration limits, failed write/readback/read, restore retry, and external policy changes; no real console access");
}
