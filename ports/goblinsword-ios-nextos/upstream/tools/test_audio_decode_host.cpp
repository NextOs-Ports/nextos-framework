#include "../prototype/audio.cpp"
#include <cassert>
#include <stdexcept>
[[noreturn]] void rt_fail(const char* reason){throw std::runtime_error(reason);}
int main(int argc,char** argv){size_t count=0,frames=0;for(int n=1;n<argc;n++){auto caf=load_caf(argv[n]);assert(caf);auto pcm=decode_caf(*caf);assert(pcm->frames()==size_t(caf->valid_frames));assert(pcm->rate==caf->format.sample_rate);assert(pcm->channels==caf->format.channels);frames+=pcm->frames();
 ExtendedFile f;f.caf=std::move(caf);f.pcm=pcm;ASBD fmt{double(pcm->rate),fourcc("lpcm"),12,pcm->channels*2,1,pcm->channels*2,pcm->channels,16,0};assert(ext_set(&f,fourcc("cfmt"),sizeof(fmt),&fmt)==0);
 std::vector<int16_t> data(pcm->samples.size());AudioBufferList buffers{1,0,{{pcm->channels,uint32_t(data.size()*2),data.data()}}};uint32_t requested=pcm->frames();assert(ext_read(&f,&requested,&buffers)==0);assert(data==pcm->samples);requested=100;assert(ext_read(&f,&requested,&buffers)==0&&requested==0);++count;
 }std::printf("PASS decoded %zu original CAF AAC files, exact packet-table timelines %zu frames, CoreAudio S16 read/EOF\n",count,frames);}
