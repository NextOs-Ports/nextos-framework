#include "../prototype/audio.cpp"
#include <AL/alext.h>
#include <cassert>
#include <stdexcept>
[[noreturn]] void rt_fail(const char* reason){throw std::runtime_error(reason);}
HostData test_host;
unsigned superclass_deallocations=0;
static void superclass_dealloc(Obj,Sel){++superclass_deallocations;}
void* rt_class(const char*){return reinterpret_cast<void*>(0x22200);}
extern "C" void* rt_lookup_super(void* pair,Sel selector){assert(static_cast<Obj*>(pair)[1]==rt_class("AVAudioPlayer"));assert(!std::strcmp(selector,"dealloc"));return reinterpret_cast<void*>(superclass_dealloc);}
HostData& rt_host(Obj){return test_host;}
bool rt_responds(Obj,Sel){return false;}
extern "C" void* rt_lookup(Obj,Sel){rt_fail("unexpected test ObjC dispatch");}
int main(int argc,char**argv){assert(argc==2);openal_library();auto open=reinterpret_cast<LPALCLOOPBACKOPENDEVICESOFT>(dlsym(openal,"alcLoopbackOpenDeviceSOFT"));auto render=reinterpret_cast<LPALCRENDERSAMPLESSOFT>(dlsym(openal,"alcRenderSamplesSOFT"));assert(open&&render);auto* dev=open(nullptr);assert(dev);const ALCint attrs[]={ALC_FORMAT_CHANNELS_SOFT,ALC_STEREO_SOFT,ALC_FORMAT_TYPE_SOFT,ALC_SHORT_SOFT,ALC_FREQUENCY,44100,0};auto* context=p_alcCreateContext(dev,attrs);assert(context);assert(p_alcMakeContextCurrent(context));
 auto caf=load_caf(argv[1]);auto* p=new Player;p->pcm=decode_caf(*caf);p->context=context;p->object=reinterpret_cast<Obj>(0x10000);test_host.native=p;players.push_back(p);
 assert(player_prepare(p->object,nullptr));assert(player_play(p->object,nullptr));std::vector<int16_t> out(8192*2);render(dev,out.data(),8192);int peak=0;for(auto x:out)peak=std::max(peak,std::abs(int(x)));assert(peak>10);
 player_pause(p->object,nullptr);assert(!player_playing(p->object,nullptr));player_set_time(p->object,nullptr,0);player_loops(p->object,nullptr,-1);assert(player_play(p->object,nullptr));render(dev,out.data(),8192);assert(player_playing(p->object,nullptr));player_stop(p->object,nullptr);assert(!player_playing(p->object,nullptr));player_dealloc(p->object,nullptr);assert(superclass_deallocations==1 && players.empty() && test_host.native==nullptr);
 unsigned interrupted=0;auto callback=[](void* value,uint32_t){++*static_cast<unsigned*>(value);};assert(session_initialize(nullptr,nullptr,callback,&interrupted)==0);assert(interruption.callback==callback&&interruption.data==&interrupted);rt_audio_tick();assert(interrupted==0);assert(session_initialize(nullptr,nullptr,nullptr,nullptr)==static_cast<Status>(fourcc("init")));
 printf("PASS system OpenAL loopback PCM output peak=%d; real AVAudioPlayer prepare/play/pause/seek/infinite-loop/stop, superclass deallocation + interruption registration without fabricated callback; no physical audio device used\n",peak);
}
