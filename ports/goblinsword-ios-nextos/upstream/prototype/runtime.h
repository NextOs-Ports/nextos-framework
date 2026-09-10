#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

using Obj = void*;
using Sel = const char*;
struct Point { double x, y; };
struct Size { double width, height; };
struct Rect { Point origin; Size size; };
struct HostData {
    std::string text;
    std::vector<Obj> array;
    std::unordered_map<std::string, Obj> dictionary;
    double number = 0;
    Rect frame{{0,0},{0,0}};
    void* native = nullptr;
    Obj target = nullptr;
    Sel action = nullptr;
    int references = 1;
};
struct MachImage;
extern MachImage* rt_image;
extern std::string rt_data_root;
extern "C" void objc_msgSend();
extern "C" void objc_msgSendSuper2();
extern "C" void* rt_lookup(Obj object, Sel selector);
extern "C" void* rt_lookup_super(void* super_info, Sel selector);
void* rt_class(const char* name);
const char* rt_class_name(Obj object);
Obj rt_new(const char* class_name);
Obj rt_alloc(Obj cls);
bool rt_responds(Obj object, Sel selector);
const char* rt_method_types(Obj object, Sel selector);
void* rt_block_copy(void* block);
void rt_block_release(void* block);
void rt_drain_main_queue();
void rt_install_base();
void rt_call_load_methods();
void rt_install_services();
void rt_services_boot();
void* rt_services_symbol(const char* symbol);
void rt_services_tick();
void rt_graphics_run_loop();
Obj rt_string(const char* value);
const char* rt_utf8(Obj value);
HostData& rt_host(Obj object);
void rt_method(const char* class_name, const char* selector, void* implementation,
               bool class_method = false);
void rt_register_image(MachImage& image);
void* rt_resolve(const char* symbol, bool weak, void* user);
void rt_install_foundation();
void rt_foundation_dealloc(Obj object);
void rt_install_idle();
void rt_idle_tick();
void rt_idle_shutdown();
void rt_install_audio();
void rt_audio_tick();
void* rt_audio_symbol(const char* symbol);
void rt_install_graphics();
void* rt_graphics_symbol(const char* symbol);
void* rt_foundation_symbol(const char* symbol);
[[noreturn]] void rt_fail(const char* reason);

template<class R, class... A> R send(Obj obj, Sel selector, A... args) {
    return reinterpret_cast<R(*)(Obj,Sel,A...)>(rt_lookup(obj,selector))(obj,selector,args...);
}
