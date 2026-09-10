#define main previous_services_fixture_main
#include "host_services_fixture.cpp"
#undef main
#include "services.cpp"
#include <sys/wait.h>
#include <sys/resource.h>
static std::vector<std::pair<Obj,bool>> pulse_edges;
static void record_pulse(void*,Obj o,float v,bool down){assert(v==(down?1.0f:0.0f));pulse_edges.push_back({o,down});}
static void write_command(const std::string& command){
    int fd=openat(diagnostic.directory_fd,"buttons",O_WRONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);assert(fd>=0);
    assert(write(fd,command.data(),command.size())==static_cast<ssize_t>(command.size()));close(fd);
}
template<class F>static void rejected(F action){
    pid_t pid=fork();assert(pid>=0);
    if(!pid){action();_exit(1);}
    int status=0;assert(waitpid(pid,&status,0)==pid&&WIFSIGNALED(status)&&WTERMSIG(status)==SIGABRT);
}
int main(){
    struct rlimit no_core{0,0};assert(setrlimit(RLIMIT_CORE,&no_core)==0);
    unsetenv("GOBLIN_DIAGNOSTIC_INPUT");diagnostic_boot();assert(!diagnostic.enabled&&diagnostic.fifo_fd==-1);
    setenv("GOBLIN_DIAGNOSTIC_INPUT","1",1);
    rt_method("NSNotificationCenter","defaultCenter",reinterpret_cast<void*>(center),true);
    rt_method("NSNotificationCenter","postNotificationName:object:",reinterpret_cast<void*>(post));
    rt_install_services();rt_services_boot();assert(diagnostic.enabled);
    assert((fcntl(diagnostic.fifo_fd,F_GETFL)&O_NONBLOCK)&&(fcntl(diagnostic.fifo_fd,F_GETFD)&FD_CLOEXEC));
    struct stat st{};assert(fstat(diagnostic.directory_fd,&st)==0&&(st.st_mode&07777)==0700&&st.st_uid==geteuid());
    assert(fstat(diagnostic.fifo_fd,&st)==0&&S_ISFIFO(st.st_mode)&&(st.st_mode&07777)==0600&&st.st_uid==geteuid());
    rt_services_tick();assert(pulse_edges.empty()); // Enabled + EOF never generates input.
    rejected([](){diagnostic_parse('a');diagnostic_parse('\n');}); // Zero controllers.
    int index=SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,6,16,1);assert(index>=0);
    SDL_Joystick*joy=SDL_JoystickOpen(index);assert(joy);
    char guid[64];SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joy),guid,sizeof(guid));
    std::string mapping=std::string(guid)+",Goblin Diagnostic Fixture,a:b0,b:b1,start:b6,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,platform:Linux,";
    assert(SDL_GameControllerAddMapping(mapping.c_str())>=0);rt_services_tick();rt_services_tick();assert(devices.size()==1);
    auto c=devices.begin()->second;Obj ext=c->extended,dp=c->dpad;
    Desc desc{0,sizeof(Block)};Block handler{nullptr,0,0,reinterpret_cast<void*>(record_pulse),&desc};
    const std::pair<const char*,Obj> tokens[]={{"a",child(ext,"buttonA")},{"b",child(ext,"buttonB")},{"up",child(dp,"up")},{"down",child(dp,"down")},{"left",child(dp,"left")},{"right",child(dp,"right")},{"start",child(ext,"buttonMenu")}};
    for(const auto& token:tokens)set_handler(token.second,nullptr,&handler);
    for(const auto& token:tokens){
        pulse_edges.clear();write_command(std::string(token.first)+"\n");rt_services_tick();
        assert((pulse_edges==std::vector<std::pair<Obj,bool>>{{token.second,true}}));assert(pressed(token.second,nullptr));
        rt_services_tick();assert((pulse_edges==std::vector<std::pair<Obj,bool>>{{token.second,true},{token.second,false}}));
        rt_services_tick();assert(pulse_edges.size()==2&&!pressed(token.second,nullptr));
    }
    pulse_edges.clear();write_command("a\nb\n");rt_services_tick();assert(pulse_edges.size()==1&&pulse_edges[0].first==tokens[0].second);
    rt_services_tick();assert(pulse_edges.size()==2&&!pulse_edges.back().second);
    rt_services_tick();assert(pulse_edges.size()==3&&pulse_edges.back()==std::make_pair(tokens[1].second,true));
    rt_services_tick();assert(pulse_edges.size()==4&&!pulse_edges.back().second);
    pulse_edges.clear();write_command("a");rt_services_tick();assert(pulse_edges.empty());
    write_command("\n");rt_services_tick();assert(pulse_edges.size()==1);rt_services_tick();assert(pulse_edges.size()==2);
    rejected([](){diagnostic_parse('\n');});
    rejected([](){diagnostic_parse('A');});
    rejected([](){diagnostic_parse('\0');});
    rejected([](){for(char ch:std::string("rightx"))diagnostic_parse(ch);});
    rejected([](){for(char ch:std::string("x\n"))diagnostic_parse(ch);});
    rejected([](){for(int i=0;i<17;++i){diagnostic_parse('a');diagnostic_parse('\n');}});
    rejected([&](){devices[c->id+100]=c;diagnostic_parse('a');diagnostic_parse('\n');});
    assert(SDL_JoystickSetVirtualButton(joy,0,1)==0);rt_services_tick();
    rejected([](){diagnostic_parse('a');diagnostic_parse('\n');});
    assert(SDL_JoystickSetVirtualButton(joy,0,0)==0);rt_services_tick();
    assert(fchmod(diagnostic.fifo_fd,0644)==0);rejected([](){diagnostic_validate();});assert(fchmod(diagnostic.fifo_fd,0600)==0);
    std::string directory=diagnostic.directory;diagnostic_cleanup();assert(access(directory.c_str(),F_OK)==-1&&errno==ENOENT);
    diagnostic=DiagnosticInput{};diagnostic_boot();directory=diagnostic.directory;
    // A substituted symlink must neither be read nor removed by module cleanup.
    assert(renameat(diagnostic.directory_fd,"buttons",diagnostic.directory_fd,"saved-fifo")==0);
    int fd=openat(diagnostic.directory_fd,"replacement",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);assert(fd>=0);close(fd);
    assert(symlinkat("replacement",diagnostic.directory_fd,"buttons")==0);rejected([](){diagnostic_validate();});
    diagnostic_cleanup();assert(lstat((directory+"/buttons").c_str(),&st)==0&&S_ISLNK(st.st_mode));
    // Fixture owns these three test entries; the service deliberately kept them.
    assert(unlink((directory+"/buttons").c_str())==0);assert(unlink((directory+"/replacement").c_str())==0);
    assert(unlink((directory+"/saved-fifo").c_str())==0);assert(rmdir(directory.c_str())==0);
    for(const auto& token:tokens)set_handler(token.second,nullptr,nullptr);
    assert(refs.empty()&&!(SDL_WasInit(0)&SDL_INIT_VIDEO));
    SDL_JoystickClose(joy);SDL_JoystickDetachVirtual(index);SDL_Quit();
    puts("PASS: opt-in only; private FIFO modes/owner/nonblock/cloexec; seven tokens; DOWN after snapshot and UP next tick; bounded serial queue; partial line/EOF; malformed/oversized/overflow rejection; zero/multiple/physically held controller rejection; symlink/mode identity rejection; cleanup only own FIFO; system SDL virtual input, no video/guest/device");
}
