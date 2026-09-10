// Directed conformance test: map original metadata but never execute guest IMPs.
#include "runtime.cpp"
#include <cassert>
#include <sys/wait.h>
extern "C" void objc_msgSend(){}
extern "C" void objc_msgSendSuper2(){}
extern "C" void rt_nil(){}
extern "C" void rt_printf_bridge(){}
extern "C" void rt_nslog_bridge(){}
void rt_foundation_dealloc(Obj){}
void rt_graphics_run_loop(){}
void*rt_graphics_symbol(const char*){return nullptr;}
void*rt_foundation_symbol(const char*){return nullptr;}
void*rt_services_symbol(const char*){return nullptr;}
void*rt_audio_symbol(const char*){return nullptr;}
static void unused_import(){}
static void* resolver(const char*n,bool,void*){
    if(!strncmp(n,"_OBJC_CLASS_$_",14))return rt_class(n+14);
    if(!strncmp(n,"_OBJC_METACLASS_$_",18))return static_cast<Class*>(rt_class(n+18))->isa;
    if(!strcmp(n,"___CFConstantStringClassReference"))return rt_class("NSConstantString");
    return reinterpret_cast<void*>(unused_import);
}
int main(int argc,char**argv){
    assert(argc==2);rt_install_base();MachImage image;std::string error;
    assert(macho_load(argv[1],resolver,nullptr,image,error));rt_register_image(image);
    // Disable +initialize in this metadata-only fixture, never in the runtime.
    for(const auto& entry:classes)initialized.insert(entry.second);
    auto get=[](const char* name){auto p=static_cast<Protocol*>(protocol_get(name));assert(p);return p;};
    auto* icade=get("iCadeEventDelegate");auto* one=get("CCTouchOneByOneDelegate");
    auto* all=get("CCTouchAllAtOnceDelegate");auto* object=get("NSObject");auto* unrelated=get("NSXMLParserDelegate");
    auto check=[&](Obj receiver,Protocol*p,bool expected){assert(send<bool>(receiver,"conformsToProtocol:",p)==expected);};
    Obj home=rt_new("HomeLayer");Obj home_class=rt_class("HomeLayer");
    for(Obj receiver:{home,home_class}){
        check(receiver,icade,true);check(receiver,one,true);check(receiver,all,true);
        check(receiver,object,true);check(receiver,unrelated,false);check(receiver,nullptr,false);
    }
    auto* subclass=static_cast<Class*>(rt_class("NXConformanceFixture"));
    subclass->super=static_cast<Class*>(home_class);subclass->isa->super=subclass->super->isa;initialized.insert(subclass);
    Obj derived=rt_alloc(subclass);for(Obj receiver:{derived,static_cast<Obj>(subclass)}){check(receiver,icade,true);check(receiver,one,true);check(receiver,unrelated,false);}
    Obj root=rt_new("NSObject");check(root,object,true);check(rt_class("NSObject"),object,true);check(root,icade,false);
    assert(!ns_conforms(nullptr,nullptr,object));assert(!ns_conforms(nullptr,nullptr,nullptr));
    // A conforming protocol inherits another protocol without requiring the
    // class to list the ancestor separately (real CCTextureProtocol metadata).
    std::unordered_set<Protocol*> visited;
    assert(protocol_conforms(get("CCTextureProtocol"),get("CCBlendProtocol"),visited));
    visited.clear();assert(!protocol_conforms(get("CCBlendProtocol"),get("CCTextureProtocol"),visited));
    pid_t child=fork();assert(child>=0);if(!child){ro(static_cast<Class*>(home_class))->protocols=reinterpret_cast<void*>(1);ns_conforms(home,nullptr,icade);_exit(1);}
    int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==90);
    // Avoid game dealloc methods; only the host allocation is released here.
    ns_dealloc(home,nullptr);ns_dealloc(derived,nullptr);ns_dealloc(root,nullptr);
    puts("{\"status\":\"PASS\",\"original_HomeLayer_protocols\":true,\"instance_and_class_receivers\":true,\"superclass_protocols\":true,\"inherited_protocols\":true,\"host_NSObject_declaration\":true,\"unrelated_false\":true,\"nil_false\":true,\"invalid_list_rejected\":true,\"executes_guest_code\":false}");
}
