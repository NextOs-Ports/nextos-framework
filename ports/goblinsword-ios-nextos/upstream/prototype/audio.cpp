#include "runtime.h"
#include <AL/al.h>
#include <AL/alc.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

// Playback-only prototype: native CocosDenshion continues to own its lifecycle.
// CAF data is parsed without modification; FFmpeg decodes the original AAC.
namespace {
constexpr uint32_t fourcc(const char* s){return (uint32_t(s[0])<<24)|(uint32_t(s[1])<<16)|(uint32_t(s[2])<<8)|uint32_t(s[3]);}
uint32_t big32(const unsigned char* p){return(uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3];}
uint64_t big64(const unsigned char* p){return(uint64_t(big32(p))<<32)|big32(p+4);}
using Status=int32_t;
struct ASBD {
    double sample_rate;
    uint32_t format,flags,bytes_per_packet,frames_per_packet,bytes_per_frame,channels,bits,reserved;
};
static_assert(sizeof(ASBD)==40,"iOS ARM64 ASBD ABI");
struct AudioBuffer {uint32_t channels,bytes;void* data;};
struct AudioBufferList {uint32_t count,padding;AudioBuffer buffers[1];};
static_assert(offsetof(AudioBufferList,buffers)==8,"iOS ARM64 AudioBufferList alignment");
struct Caf {
    std::string path;std::vector<unsigned char> file;ASBD format{};
    size_t data_offset=0,data_bytes=0;int64_t valid_frames=0,packets=0;
    uint32_t priming_frames=0,remainder_frames=0,edit_counter=0;
};
std::string url_path(Obj url){
    if(!url)rt_fail("null audio URL");
    Obj path=send<Obj>(url,"path");
    const char* p=rt_utf8(path);if(!p||!*p)rt_fail("empty audio file URL");return p;
}
std::unique_ptr<Caf> load_caf(const std::string& path){
    auto c=std::make_unique<Caf>();c->path=path;std::ifstream file(path,std::ios::binary);
    if(!file){std::fprintf(stderr,"[audio] open failed: %s\n",path.c_str());return {};}
    c->file.assign(std::istreambuf_iterator<char>(file),{});const auto& b=c->file;
    if(b.size()<8||std::memcmp(b.data(),"caff\0\1\0\0",8))rt_fail("audio input is not supported CAF version 1");
    bool desc=false,data=false;
    for(size_t off=8;off+12<=b.size();){
        uint32_t type=big32(b.data()+off);int64_t signed_size=static_cast<int64_t>(big64(b.data()+off+4));
        if(signed_size<0&&!(type==fourcc("data")&&signed_size==-1))rt_fail("invalid negative CAF chunk length");
        size_t n=signed_size==-1?b.size()-off-12:size_t(signed_size);
        if(n>b.size()-off-12)rt_fail("CAF chunk exceeds file");
        const auto* p=b.data()+off+12;
        if(type==fourcc("desc")){
            if(desc||n!=32)rt_fail("invalid CAF description chunk");
            desc=true;
            uint64_t bits=big64(p);std::memcpy(&c->format.sample_rate,&bits,8);
            c->format.format=big32(p+8);c->format.flags=big32(p+12);c->format.bytes_per_packet=big32(p+16);
            c->format.frames_per_packet=big32(p+20);c->format.channels=big32(p+24);c->format.bits=big32(p+28);
            c->format.bytes_per_frame=c->format.frames_per_packet?c->format.bytes_per_packet/c->format.frames_per_packet:0;
            if(!std::isfinite(c->format.sample_rate)||c->format.sample_rate<8000||c->format.sample_rate>192000||c->format.channels<1||c->format.channels>2)rt_fail("CAF sample rate/channel count unsupported");
        } else if(type==fourcc("data")){
            if(data||n<4)rt_fail("invalid CAF audio data chunk");
            data=true;c->data_offset=off+16;c->data_bytes=n-4;
            // Edit counter is revision metadata, not an audio edit list.
            c->edit_counter=big32(p);
        } else if(type==fourcc("pakt")){
            if(n<24)rt_fail("short CAF packet table");
            c->packets=big64(p);c->valid_frames=big64(p+8);c->priming_frames=big32(p+16);c->remainder_frames=big32(p+20);
            if(c->packets<0||c->valid_frames<0)rt_fail("invalid CAF packet/frame count");
        }
        off+=n+12;
    }
    if(!desc||!data)rt_fail("CAF missing required description/data");
    if(c->format.format!=fourcc("aac ")&&c->format.format!=fourcc("lpcm"))rt_fail("unsupported CAF codec");
    if(c->format.format==fourcc("lpcm")&&!c->valid_frames&&c->format.bytes_per_frame)c->valid_frames=c->data_bytes/c->format.bytes_per_frame;
    return c;
}
struct Pcm {unsigned rate=0,channels=0;std::vector<int16_t> samples;size_t frames()const{return channels?samples.size()/channels:0;}};
void avcheck(int result,const char* operation){if(result>=0)return;char error[AV_ERROR_MAX_STRING_SIZE]{};av_strerror(result,error,sizeof(error));std::fprintf(stderr,"[audio] %s: %s\n",operation,error);rt_fail(operation);}
std::shared_ptr<Pcm> decode_caf(const Caf& caf){
    AVFormatContext* format=nullptr;avcheck(avformat_open_input(&format,caf.path.c_str(),nullptr,nullptr),"FFmpeg CAF open");
    avcheck(avformat_find_stream_info(format,nullptr),"FFmpeg CAF stream info");
    const AVCodec* codec=nullptr;int stream=av_find_best_stream(format,AVMEDIA_TYPE_AUDIO,-1,-1,&codec,0);avcheck(stream,"FFmpeg audio stream");
    auto* decoder=avcodec_alloc_context3(codec);if(!decoder)rt_fail("FFmpeg decoder allocation");
    avcheck(avcodec_parameters_to_context(decoder,format->streams[stream]->codecpar),"FFmpeg decoder parameters");avcheck(avcodec_open2(decoder,codec,nullptr),"FFmpeg decoder open");
    auto pcm=std::make_shared<Pcm>();pcm->rate=std::lround(caf.format.sample_rate);pcm->channels=caf.format.channels;
    AVChannelLayout layout{};av_channel_layout_default(&layout,pcm->channels);
    SwrContext* resampler=nullptr;avcheck(swr_alloc_set_opts2(&resampler,&layout,AV_SAMPLE_FMT_S16,pcm->rate,&decoder->ch_layout,decoder->sample_fmt,decoder->sample_rate,0,nullptr),"FFmpeg resampler allocation");avcheck(swr_init(resampler),"FFmpeg resampler init");
    AVFrame* frame=av_frame_alloc();AVPacket* packet=av_packet_alloc();if(!frame||!packet)rt_fail("FFmpeg frame/packet allocation");
    auto receive=[&](){
        for(;;){int result=avcodec_receive_frame(decoder,frame);if(result==AVERROR(EAGAIN)||result==AVERROR_EOF)return;avcheck(result,"FFmpeg AAC decode");
            int capacity=swr_get_out_samples(resampler,frame->nb_samples);if(capacity<0)rt_fail("invalid resampler output size");
            std::vector<int16_t> output(size_t(capacity)*pcm->channels);uint8_t* out=reinterpret_cast<uint8_t*>(output.data());
            int count=swr_convert(resampler,&out,capacity,const_cast<const uint8_t**>(frame->extended_data),frame->nb_samples);avcheck(count,"FFmpeg PCM conversion");
            if(pcm->samples.size()+size_t(count)*pcm->channels>128UL*1024*1024)rt_fail("decoded audio exceeds 256 MiB prototype bound");
            pcm->samples.insert(pcm->samples.end(),output.begin(),output.begin()+size_t(count)*pcm->channels);av_frame_unref(frame);
        }
    };
    for(;;){int result=av_read_frame(format,packet);if(result==AVERROR_EOF)break;avcheck(result,"FFmpeg CAF packet read");if(packet->stream_index==stream){avcheck(avcodec_send_packet(decoder,packet),"FFmpeg AAC packet submit");receive();}av_packet_unref(packet);}
    avcheck(avcodec_send_packet(decoder,nullptr),"FFmpeg decoder drain");receive();
    for(;;){int capacity=swr_get_out_samples(resampler,0);if(capacity<=0)break;std::vector<int16_t> output(size_t(capacity)*pcm->channels);uint8_t* out=reinterpret_cast<uint8_t*>(output.data());int count=swr_convert(resampler,&out,capacity,nullptr,0);avcheck(count,"FFmpeg resampler drain");if(!count)break;pcm->samples.insert(pcm->samples.end(),output.begin(),output.begin()+size_t(count)*pcm->channels);}
    av_packet_free(&packet);av_frame_free(&frame);swr_free(&resampler);av_channel_layout_uninit(&layout);avcodec_free_context(&decoder);avformat_close_input(&format);
    // CAF packet table gives the valid timeline. Current FFmpeg may return AAC
    // padding, depending on its demuxer/decoder; trim only a fully identified case.
    size_t valid=size_t(caf.valid_frames),decoded=pcm->frames();
    if(valid&&decoded!=valid){
        size_t start=0;
        if(decoded==valid+size_t(caf.priming_frames)+caf.remainder_frames)start=caf.priming_frames;
        else if(decoded!=valid+size_t(caf.remainder_frames)){
            std::fprintf(stderr,"[audio] CAF valid=%zu priming=%u remainder=%u decoded=%zu\n",valid,caf.priming_frames,caf.remainder_frames,decoded);rt_fail("CAF decoder timeline mismatch");
        }
        std::vector<int16_t> trimmed(pcm->samples.begin()+start*pcm->channels,pcm->samples.begin()+(start+valid)*pcm->channels);pcm->samples.swap(trimmed);
    }
    if(pcm->samples.empty())rt_fail("CAF decode yielded no PCM");
    int peak=0;for(int16_t sample:pcm->samples)peak=std::max(peak,std::abs(int(sample)));
    std::fprintf(stderr,"[audio] decoded %s codec=%c%c%c%c rate=%u channels=%u frames=%zu peak=%d\n",caf.path.c_str(),caf.format.format>>24,caf.format.format>>16&255,caf.format.format>>8&255,caf.format.format&255,pcm->rate,pcm->channels,pcm->frames(),peak);
    return pcm;
}
struct ExtendedFile {std::unique_ptr<Caf> caf;std::shared_ptr<Pcm> pcm;ASBD client{};size_t cursor=0;bool configured=false;};
template<class T>Status property_copy(uint32_t* size,void* output,const T& value){if(!size||!output||*size<sizeof(T))return -50;std::memcpy(output,&value,sizeof(T));*size=sizeof(T);return 0;}
[[noreturn]]void unknown_property(const char* api,uint32_t property){std::fprintf(stderr,"[audio] unsupported %s property %c%c%c%c (0x%08x)\n",api,property>>24,property>>16&255,property>>8&255,property&255,property);rt_fail("unimplemented CoreAudio property");}
Status ext_open(Obj url,ExtendedFile** out){if(!out)return -50;*out=nullptr;auto caf=load_caf(url_path(url));if(!caf)return -43;auto* f=new ExtendedFile;f->pcm=decode_caf(*caf);f->caf=std::move(caf);*out=f;return 0;}
Status ext_dispose(ExtendedFile* f){delete f;return 0;}
Status ext_get(ExtendedFile* f,uint32_t p,uint32_t* size,void* data){if(!f)return -50;if(p==fourcc("ffmt"))return property_copy(size,data,f->caf->format);if(p==fourcc("#frm")){int64_t frames=f->pcm->frames();return property_copy(size,data,frames);}if(p==fourcc("cfmt")){if(!f->configured)return -50;return property_copy(size,data,f->client);}unknown_property("ExtAudioFileGetProperty",p);}
Status ext_set(ExtendedFile* f,uint32_t p,uint32_t size,const void* data){
    if(!f||!data)return -50;
    if(p!=fourcc("cfmt"))unknown_property("ExtAudioFileSetProperty",p);
    if(size!=sizeof(ASBD))return -50;
    ASBD a{};std::memcpy(&a,data,sizeof(a));
    if(a.format!=fourcc("lpcm")||a.flags!=12||a.channels!=f->pcm->channels||a.sample_rate!=f->pcm->rate||a.bits!=16||a.frames_per_packet!=1||a.bytes_per_frame!=a.channels*2||a.bytes_per_packet!=a.bytes_per_frame)rt_fail("ExtAudioFile supports requested native-rate interleaved signed packed S16 only");
    f->client=a;f->configured=true;return 0;
}
Status ext_read(ExtendedFile* f,uint32_t* count,AudioBufferList* list){
    if(!f||!count||!list||!f->configured)return -50;
    if(list->count!=1||list->buffers[0].channels!=f->pcm->channels||(!list->buffers[0].data&&*count))return -50;
    size_t frames=std::min<size_t>(*count,f->pcm->frames()-f->cursor),bytes=frames*f->client.bytes_per_frame;
    if(bytes>list->buffers[0].bytes)return -50;
    std::memcpy(list->buffers[0].data,f->pcm->samples.data()+f->cursor*f->pcm->channels,bytes);f->cursor+=frames;*count=frames;list->buffers[0].bytes=bytes;return 0;
}
Status file_open(Obj url,unsigned char permissions,uint32_t hint,Caf** out){if(!out||permissions!=1)return -50;*out=nullptr;if(hint&&hint!=fourcc("caff"))rt_fail("AudioFile type hint unsupported");auto c=load_caf(url_path(url));if(!c)return -43;*out=c.release();return 0;}
Status file_close(Caf* file){delete file;return 0;}
Status file_get(Caf* f,uint32_t p,uint32_t* size,void* data){if(!f)return -50;if(p==fourcc("dfmt"))return property_copy(size,data,f->format);if(p==fourcc("bcnt")){uint64_t n=f->data_bytes;return property_copy(size,data,n);}if(p==fourcc("pcnt")){uint64_t n=f->packets;return property_copy(size,data,n);}unknown_property("AudioFileGetProperty",p);}
Status file_read(Caf* f,bool,int64_t start,uint32_t* size,void* buffer){if(!f||!size||!buffer||start<0||uint64_t(start)>f->data_bytes)return -50;uint32_t n=std::min<uint64_t>(*size,f->data_bytes-start);std::memcpy(buffer,f->file.data()+f->data_offset+start,n);*size=n;return 0;}

void* openal=nullptr;
#define AL_FUNCTION(name) decltype(&name) p_##name=nullptr
AL_FUNCTION(alcOpenDevice);AL_FUNCTION(alcCloseDevice);AL_FUNCTION(alcCreateContext);AL_FUNCTION(alcGetCurrentContext);AL_FUNCTION(alcGetContextsDevice);AL_FUNCTION(alcMakeContextCurrent);AL_FUNCTION(alcGetString);
AL_FUNCTION(alGenBuffers);AL_FUNCTION(alBufferData);AL_FUNCTION(alGenSources);AL_FUNCTION(alSourcei);AL_FUNCTION(alSourcef);AL_FUNCTION(alSourcePlay);AL_FUNCTION(alSourcePause);AL_FUNCTION(alSourceStop);AL_FUNCTION(alSourceRewind);AL_FUNCTION(alDeleteSources);AL_FUNCTION(alDeleteBuffers);AL_FUNCTION(alGetSourcei);AL_FUNCTION(alGetSourcef);AL_FUNCTION(alGetError);
void openal_library(){
    if(openal)return;
    openal=dlopen("libopenal.so.1",RTLD_NOW|RTLD_LOCAL);if(!openal)rt_fail("system libopenal.so.1 unavailable");
#define LOAD_AL(n) p_##n=reinterpret_cast<decltype(p_##n)>(dlsym(openal,#n));if(!p_##n)rt_fail(#n " missing from system OpenAL")
    LOAD_AL(alcOpenDevice);LOAD_AL(alcCloseDevice);LOAD_AL(alcCreateContext);LOAD_AL(alcGetCurrentContext);LOAD_AL(alcGetContextsDevice);LOAD_AL(alcMakeContextCurrent);LOAD_AL(alcGetString);
    LOAD_AL(alGenBuffers);LOAD_AL(alBufferData);LOAD_AL(alGenSources);LOAD_AL(alSourcei);LOAD_AL(alSourcef);LOAD_AL(alSourcePlay);LOAD_AL(alSourcePause);LOAD_AL(alSourceStop);LOAD_AL(alSourceRewind);LOAD_AL(alDeleteSources);LOAD_AL(alDeleteBuffers);LOAD_AL(alGetSourcei);LOAD_AL(alGetSourcef);LOAD_AL(alGetError);
}
void al_check(const char* operation){ALenum err=p_alGetError();if(err){std::fprintf(stderr,"[audio] %s OpenAL error=0x%x\n",operation,err);rt_fail(operation);}}
ALCdevice* guest_open_device(const ALCchar* name){openal_library();auto* device=p_alcOpenDevice(name);std::fprintf(stderr,"[audio] native alcOpenDevice returned=%p device=%s\n",static_cast<void*>(device),device?p_alcGetString(device,ALC_DEVICE_SPECIFIER):"unavailable");return device;}
ALCcontext* player_context(){
    openal_library();if(auto* c=p_alcGetCurrentContext())return c;
    static ALCcontext* own=nullptr;if(!own){auto* d=p_alcOpenDevice(nullptr);if(!d)rt_fail("no real OpenAL output device for AVAudioPlayer");own=p_alcCreateContext(d,nullptr);if(!own)rt_fail("AVAudioPlayer OpenAL context creation failed");}
    return own;
}
struct ScopedContext {ALCcontext* previous;explicit ScopedContext(ALCcontext* c):previous(p_alcGetCurrentContext()){if(!p_alcMakeContextCurrent(c))rt_fail("AVAudioPlayer context activation failed");}~ScopedContext(){if(!p_alcMakeContextCurrent(previous))rt_fail("AVAudioPlayer restoring guest OpenAL context failed");}};
struct Player {std::shared_ptr<Pcm> pcm;ALCcontext* context=nullptr;ALuint source=0,buffer=0;Obj object=nullptr,delegate=nullptr;long loops=0,left=0;float volume=1;bool started=false,paused=false;};
std::vector<Player*> players;
Player& player(Obj o){auto* p=static_cast<Player*>(rt_host(o).native);if(!p)rt_fail("uninitialized AVAudioPlayer");return *p;}
Obj player_init(Obj o,Sel,Obj url,Obj* error){if(error)*error=nullptr;auto caf=load_caf(url_path(url));if(!caf){if(error)*error=rt_new("NSError");return nullptr;}auto* p=new Player;p->object=o;p->pcm=decode_caf(*caf);p->context=player_context();rt_host(o).native=p;players.push_back(p);return o;}
bool player_prepare(Obj o,Sel){
    auto& p=player(o);ScopedContext scope(p.context);if(p.buffer)return true;
    p_alGenBuffers(1,&p.buffer);p_alBufferData(p.buffer,p.pcm->channels==1?AL_FORMAT_MONO16:AL_FORMAT_STEREO16,p.pcm->samples.data(),p.pcm->samples.size()*sizeof(int16_t),p.pcm->rate);al_check("AVAudioPlayer PCM upload");
    p_alGenSources(1,&p.source);p_alSourcei(p.source,AL_BUFFER,p.buffer);p_alSourcei(p.source,AL_SOURCE_RELATIVE,AL_TRUE);p_alSourcef(p.source,AL_ROLLOFF_FACTOR,0);p_alSourcef(p.source,AL_GAIN,p.volume);p_alSourcei(p.source,AL_LOOPING,p.loops<0);al_check("AVAudioPlayer source creation");return true;
}
bool player_play(Obj o,Sel){auto& p=player(o);player_prepare(o,nullptr);ScopedContext scope(p.context);if(!p.paused)p.left=p.loops;p_alSourcePlay(p.source);al_check("AVAudioPlayer play");p.started=true;p.paused=false;ALint state=0;p_alGetSourcei(p.source,AL_SOURCE_STATE,&state);std::fprintf(stderr,"[audio] AVAudioPlayer source=%u play_state=0x%x (device output, not audible validation)\n",p.source,state);return state==AL_PLAYING;}
void player_pause(Obj o,Sel){auto& p=player(o);if(p.source){ScopedContext scope(p.context);p_alSourcePause(p.source);al_check("AVAudioPlayer pause");p.paused=true;}}
void player_stop(Obj o,Sel){auto& p=player(o);if(p.source){ScopedContext scope(p.context);p_alSourceStop(p.source);p_alSourceRewind(p.source);al_check("AVAudioPlayer stop");}p.started=false;p.paused=false;}
bool player_playing(Obj o,Sel){auto& p=player(o);if(!p.source)return false;ScopedContext scope(p.context);ALint state=0;p_alGetSourcei(p.source,AL_SOURCE_STATE,&state);al_check("AVAudioPlayer state");return state==AL_PLAYING;}
void player_volume(Obj o,Sel,float n){if(!std::isfinite(n))rt_fail("invalid AVAudioPlayer gain");auto& p=player(o);p.volume=std::clamp(n,0.0f,1.0f);if(p.source){ScopedContext scope(p.context);p_alSourcef(p.source,AL_GAIN,p.volume);al_check("AVAudioPlayer volume");}}
float get_volume(Obj o,Sel){return player(o).volume;}
void player_loops(Obj o,Sel,long n){auto& p=player(o);p.loops=n;p.left=n;if(p.source){ScopedContext scope(p.context);p_alSourcei(p.source,AL_LOOPING,n<0);al_check("AVAudioPlayer loops");}}
long get_loops(Obj o,Sel){return player(o).loops;}
void player_delegate(Obj o,Sel,Obj d){player(o).delegate=d;}
Obj get_player_delegate(Obj o,Sel){return player(o).delegate;}
double player_duration(Obj o,Sel){auto& p=player(o);return double(p.pcm->frames())/p.pcm->rate;}
double player_time(Obj o,Sel){auto& p=player(o);if(!p.source)return 0;ScopedContext scope(p.context);ALfloat n=0;p_alGetSourcef(p.source,AL_SEC_OFFSET,&n);al_check("AVAudioPlayer currentTime");return n;}
void player_set_time(Obj o,Sel,double n){auto& p=player(o);if(!std::isfinite(n)||n<0||n>player_duration(o,nullptr))rt_fail("invalid AVAudioPlayer seek");player_prepare(o,nullptr);ScopedContext scope(p.context);p_alSourcef(p.source,AL_SEC_OFFSET,float(n));al_check("AVAudioPlayer seek");}
void player_dealloc(Obj o,Sel){
    auto* p=static_cast<Player*>(rt_host(o).native);
    if(p){
        {ScopedContext scope(p->context);if(p->source)p_alDeleteSources(1,&p->source);if(p->buffer)p_alDeleteBuffers(1,&p->buffer);al_check("AVAudioPlayer cleanup");}
        players.erase(std::remove(players.begin(),players.end(),p),players.end());rt_host(o).native=nullptr;delete p;
    }
    // objc_msgSendSuper2 starts searching above the defining AVAudioPlayer class.
    Obj super_info[]={o,rt_class("AVAudioPlayer")};
    reinterpret_cast<void(*)(Obj,Sel)>(rt_lookup_super(super_info,"dealloc"))(o,"dealloc");
}

Obj session=nullptr,ambient=nullptr,solo_ambient=nullptr,playback=nullptr,play_record=nullptr;
uint32_t session_category=fourcc("solo");bool session_active=false;
struct InterruptionRegistration {void* runloop=nullptr;Obj mode=nullptr;void(*callback)(void*,uint32_t)=nullptr;void* data=nullptr;bool initialized=false;};
InterruptionRegistration interruption;
std::mutex interruption_mutex;
Obj shared_session(Obj,Sel){if(!session)session=rt_new("AVAudioSession");return session;}
void session_delegate(Obj o,Sel,Obj d){rt_host(o).dictionary["delegate"]=d;}
bool category_supported(uint32_t c){return c==fourcc("ambi")||c==fourcc("solo")||c==fourcc("medi");}
Status session_initialize(void* runloop,Obj mode,void(*callback)(void*,uint32_t),void* data){
    openal_library();std::lock_guard<std::mutex> lock(interruption_mutex);
    if(interruption.initialized)return static_cast<Status>(fourcc("init"));
    interruption={runloop,mode,callback,data,true};
    // Registration does not imply an interruption. This playback-only host has
    // no supported interruption event source yet, so it never fabricates calls.
    std::fprintf(stderr,"[audio] AudioSession initialized callback_registered=%s; no synthetic interruption events\n",callback?"yes":"no");
    return 0;
}
Status session_set(uint32_t property,uint32_t size,const void* value){
    if(property==fourcc("acat")){if(!value||size!=4)return -50;uint32_t c;std::memcpy(&c,value,4);if(!category_supported(c))rt_fail("AudioSession category requires unsupported recording/processing");session_category=c;return 0;}unknown_property("AudioSessionSetProperty",property);
}
Status session_activate(bool active){
    openal_library();if(active&&!category_supported(session_category))rt_fail("unsupported AudioSession active category");
    if(!active)rt_fail("AudioSession deactivation requires coordinated guest/AVAudioPlayer source suspension; not implemented");
    // Activation is policy state; actual hardware success is checked at alcOpenDevice.
    session_active=active;return 0;
}
Status session_get(uint32_t property,uint32_t* size,void* value){
    if(property==fourcc("othr")){// No other iOS audio clients exist inside this emulated process/session.
        uint32_t other=0;return property_copy(size,value,other);
    }
    if(property==fourcc("rout")){openal_library();Obj route=rt_string(p_alcGetCurrentContext()?"Speaker":"");return property_copy(size,value,route);}
    if(property==fourcc("acat"))return property_copy(size,value,session_category);
    unknown_property("AudioSessionGetProperty",property);
}
bool set_category(Obj,Sel,Obj category,Obj* error){if(error)*error=nullptr;const char* c=rt_utf8(category);if(!std::strcmp(c,"AVAudioSessionCategoryAmbient"))session_category=fourcc("ambi");else if(!std::strcmp(c,"AVAudioSessionCategorySoloAmbient"))session_category=fourcc("solo");else if(!std::strcmp(c,"AVAudioSessionCategoryPlayback"))session_category=fourcc("medi");else rt_fail("AVAudioSession category unsupported");return true;}
bool set_active(Obj,Sel,bool active,Obj* error){if(error)*error=nullptr;return session_activate(active)==0;}
Obj get_category(Obj,Sel){return session_category==fourcc("ambi")?ambient:session_category==fourcc("medi")?playback:solo_ambient;}
bool other_audio(Obj,Sel){return false;}
}

void rt_audio_tick(){
    auto snapshot=players;
    for(Player* p:snapshot){if(std::find(players.begin(),players.end(),p)==players.end()||!p->started||p->paused||!p->source||p->loops<0)continue;ALint state=0;{ScopedContext scope(p->context);p_alGetSourcei(p->source,AL_SOURCE_STATE,&state);al_check("AVAudioPlayer completion polling");if(state!=AL_STOPPED)continue;if(p->left>0){--p->left;p_alSourcePlay(p->source);al_check("AVAudioPlayer finite repeat");continue;}}
        p->started=false;if(p->delegate&&rt_responds(p->delegate,"audioPlayerDidFinishPlaying:successfully:"))send<void>(p->delegate,"audioPlayerDidFinishPlaying:successfully:",p->object,true);
    }
}
void rt_install_audio(){
#define METHOD(c,s,f) rt_method(c,s,reinterpret_cast<void*>(f))
#define CLASS(c,s,f) rt_method(c,s,reinterpret_cast<void*>(f),true)
    METHOD("AVAudioPlayer","initWithContentsOfURL:error:",player_init);METHOD("AVAudioPlayer","prepareToPlay",player_prepare);METHOD("AVAudioPlayer","play",player_play);METHOD("AVAudioPlayer","pause",player_pause);METHOD("AVAudioPlayer","stop",player_stop);METHOD("AVAudioPlayer","isPlaying",player_playing);
    METHOD("AVAudioPlayer","setVolume:",player_volume);METHOD("AVAudioPlayer","volume",get_volume);METHOD("AVAudioPlayer","setNumberOfLoops:",player_loops);METHOD("AVAudioPlayer","numberOfLoops",get_loops);METHOD("AVAudioPlayer","setDelegate:",player_delegate);METHOD("AVAudioPlayer","delegate",get_player_delegate);METHOD("AVAudioPlayer","duration",player_duration);METHOD("AVAudioPlayer","currentTime",player_time);METHOD("AVAudioPlayer","setCurrentTime:",player_set_time);METHOD("AVAudioPlayer","dealloc",player_dealloc);
    CLASS("AVAudioSession","sharedInstance",shared_session);METHOD("AVAudioSession","setDelegate:",session_delegate);METHOD("AVAudioSession","setCategory:error:",set_category);METHOD("AVAudioSession","setActive:error:",set_active);METHOD("AVAudioSession","category",get_category);METHOD("AVAudioSession","isOtherAudioPlaying",other_audio);
    ambient=rt_string("AVAudioSessionCategoryAmbient");solo_ambient=rt_string("AVAudioSessionCategorySoloAmbient");playback=rt_string("AVAudioSessionCategoryPlayback");play_record=rt_string("AVAudioSessionCategoryPlayAndRecord");
}
void* rt_audio_symbol(const char* s){
#define SYMBOL(n,f) if(!std::strcmp(s,n))return reinterpret_cast<void*>(f)
    SYMBOL("_AudioFileOpenURL",file_open);SYMBOL("_AudioFileClose",file_close);SYMBOL("_AudioFileGetProperty",file_get);SYMBOL("_AudioFileReadBytes",file_read);
    SYMBOL("_ExtAudioFileOpenURL",ext_open);SYMBOL("_ExtAudioFileDispose",ext_dispose);SYMBOL("_ExtAudioFileGetProperty",ext_get);SYMBOL("_ExtAudioFileSetProperty",ext_set);SYMBOL("_ExtAudioFileRead",ext_read);
    SYMBOL("_AudioSessionInitialize",session_initialize);SYMBOL("_AudioSessionSetProperty",session_set);SYMBOL("_AudioSessionGetProperty",session_get);SYMBOL("_AudioSessionSetActive",session_activate);
    SYMBOL("_AVAudioSessionCategoryAmbient",&ambient);SYMBOL("_AVAudioSessionCategorySoloAmbient",&solo_ambient);SYMBOL("_AVAudioSessionCategoryPlayback",&playback);SYMBOL("_AVAudioSessionCategoryPlayAndRecord",&play_record);
    SYMBOL("_alcOpenDevice",guest_open_device);
    if(!std::strncmp(s,"_al",3)){openal_library();return dlsym(openal,s+1);}return nullptr;
}
