// Narrow CPU benchmark of the current runtime; no Mach-O code or GPU execution.
#define main existing_runtime_fixture_main
#include "host_runtime_fixture.cpp"
#undef main
#include <iomanip>
static volatile uintptr_t benchmark_sink=0;
static std::vector<std::unique_ptr<unsigned char[]>> method_storage;
static std::vector<std::unique_ptr<char[]>> name_storage;
static const char* name_copy(const std::string&s){auto p=std::make_unique<char[]>(s.size()+1);memcpy(p.get(),s.c_str(),s.size()+1);auto result=p.get();name_storage.push_back(std::move(p));return result;}
static const char* populate(Class*c,unsigned count){
    auto memory=std::make_unique<unsigned char[]>(8+count*sizeof(Method));
    auto methods=reinterpret_cast<Methods*>(memory.get());methods->size=sizeof(Method);methods->count=count;
    const char* last=nullptr;
    for(unsigned i=0;i<count;++i){last=name_copy("benchmarkSelectorWithObject"+std::to_string(i)+":");
        auto entry=reinterpret_cast<Method*>(memory.get()+8+i*sizeof(Method));*entry={last,"v24@0:8@16",reinterpret_cast<void*>(unused_import)};}
    ro(c)->methods=methods;method_storage.push_back(std::move(memory));return last;
}
template<class F>static void measure(const char*name,size_t n,const char*unit,F fn){
    auto start=std::chrono::steady_clock::now();
    for(size_t i=0;i<n;++i){asm volatile("" : : : "memory");benchmark_sink^=fn();}
    auto end=std::chrono::steady_clock::now();
    double ns=std::chrono::duration<double,std::nano>(end-start).count()/n;
    printf("%s,%zu,%.3f,%s\n",name,n,ns,unit);
}
static size_t present_pixel_count(const std::vector<unsigned char>&pixels){
    size_t lit=0;for(size_t i=0;i<pixels.size();i+=4)if(pixels[i]>3||pixels[i+1]>3||pixels[i+2]>3)++lit;return lit;
}
int main(){
    rt_install_base();
    auto c64=static_cast<Class*>(rt_class("BenchmarkSyntheticClassWith64Methods"));auto last64=populate(c64,64);Obj o64=rt_alloc(c64);
    auto c256=static_cast<Class*>(rt_class("BenchmarkSyntheticClassWith256Methods"));auto last256=populate(c256,256);Obj o256=rt_alloc(c256);
    auto leaf=c64;for(unsigned i=0;i<4;++i){auto c=static_cast<Class*>(rt_class(("BenchmarkEmptySubclass"+std::to_string(i)).c_str()));c->super=leaf;leaf=c;}
    Obj deep=rt_alloc(leaf);
    // Warm initialization before measuring; only synthetic host IMPs exist here.
    assert(rt_lookup(o64,last64)==reinterpret_cast<void*>(unused_import));
    assert(rt_lookup(o256,last256)==reinterpret_cast<void*>(unused_import));
    assert(rt_lookup(deep,last64)==reinterpret_cast<void*>(unused_import));
    puts("case,iterations,ns_per_operation,unit");
    measure("own_method_64_last",100000,"lookup",[&](){return reinterpret_cast<uintptr_t>(own_method(c64,last64));});
    measure("rt_lookup_64_first",100000,"lookup",[&](){return reinterpret_cast<uintptr_t>(rt_lookup(o64,ro(c64)->methods->entries[0].name));});
    measure("rt_lookup_64_last",100000,"lookup",[&](){return reinterpret_cast<uintptr_t>(rt_lookup(o64,last64));});
    measure("rt_lookup_256_last",100000,"lookup",[&](){return reinterpret_cast<uintptr_t>(rt_lookup(o256,last256));});
    measure("rt_lookup_64_last_four_empty_subclasses",100000,"lookup",[&](){return reinterpret_cast<uintptr_t>(rt_lookup(deep,last64));});
    measure("rt_lookup_NSObject_override_after_five_classes",100000,"lookup",[&](){return reinterpret_cast<uintptr_t>(rt_lookup(deep,"retain"));});
    std::vector<unsigned char> black(size_t(1280)*720*4),colored(black.size(),255);
    assert(present_pixel_count(black)==0&&present_pixel_count(colored)==1280*720);
    measure("present_full_cpu_scan_black_1280x720",40,"frame",[&](){return present_pixel_count(black);});
    measure("present_full_cpu_scan_colored_1280x720",40,"frame",[&](){return present_pixel_count(colored);});
    measure("present_RGBA_vector_allocate_zero_free_1280x720",40,"frame",[&](){std::vector<unsigned char> pixels(black.size());asm volatile("" : : "r"(pixels.data()) : "memory");return pixels.size();});
    // Free fixture instances. Process-lifetime class metadata follows runtime policy.
    ns_release(o64,nullptr);ns_release(o256,nullptr);ns_release(deep,nullptr);
}
