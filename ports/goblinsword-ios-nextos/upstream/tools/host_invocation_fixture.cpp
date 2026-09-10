// Native metadata + host callbacks only: does not execute ARM64 guest code.
#include "runtime.cpp"
#include "foundation.cpp"
#include <cassert>
#include <sys/wait.h>
extern "C" void objc_msgSend(){}
extern "C" void objc_msgSendSuper2(){}
extern "C" void rt_nil(){}
extern "C" void rt_printf_bridge(){}
extern "C" void rt_nslog_bridge(){}
extern "C" void foundation_string_format(){}
extern "C" void foundation_array_objects(){}
extern "C" void foundation_dictionary_objects_keys(){}
void rt_graphics_run_loop(){}
void*rt_graphics_symbol(const char*){return nullptr;}
void*rt_services_symbol(const char*){return nullptr;}
void*rt_audio_symbol(const char*){return nullptr;}
static void unused_import(){}
static void* resolver(const char*n,bool,void*){
    if(!strncmp(n,"_OBJC_CLASS_$_",14))return rt_class(n+14);
    if(!strncmp(n,"_OBJC_METACLASS_$_",18))return static_cast<Class*>(rt_class(n+18))->isa;
    if(!strcmp(n,"___CFConstantStringClassReference"))return rt_class("NSConstantString");
    return reinterpret_cast<void*>(unused_import);
}
static Obj observed_target=nullptr,observed_sender=nullptr;
static Sel observed_selector=nullptr;
static unsigned calls=0;
static void callback(Obj target,Sel selector,Obj sender){observed_target=target;observed_selector=selector;observed_sender=sender;++calls;}
int main(int argc,char**argv){
    assert(argc==2);rt_install_base();rt_install_foundation();MachImage image;std::string error;
    assert(macho_load(argv[1],resolver,nullptr,image,error));rt_register_image(image);
    // This fixture explicitly suppresses guest +initialize, never the runtime.
    for(const auto& entry:classes)initialized.insert(entry.second);
    Obj target=rt_new("GameLayer"),sender=rt_new("NSObject"),other=rt_new("NSObject");
    for(Sel sel:{"pausePressed:","blankPressed:","jumpButtonPressed:","attackButtonPressed:","dashPressed:","leftButtonPressed:","rightButtonPressed:"}){
        const char* encoding=rt_method_types(target,sel);assert(encoding&&!strcmp(encoding,"v24@0:8@16"));
        Obj signature=send<Obj>(target,"methodSignatureForSelector:",sel);assert(signature&&rt_host(signature).text==encoding);
        class_replace(static_cast<Class*>(rt_class("GameLayer")),sel,reinterpret_cast<void*>(callback),encoding);
        Obj invocation=send<Obj>(rt_class("NSInvocation"),"invocationWithMethodSignature:",signature);
        send<void>(invocation,"setTarget:",target);send<void>(invocation,"setSelector:",sel);
        Obj argument=sender;send<void>(invocation,"setArgument:atIndex:",static_cast<const void*>(&argument),uint64_t(2));argument=other;
        send<void>(invocation,"invoke");assert(observed_target==target&&observed_sender==sender&&!strcmp(observed_selector,sel));
        assert(rt_host(target).references==1&&rt_host(sender).references==1&&rt_host(signature).references==2);
    }
    assert(calls==7);
    assert(!send<Obj>(target,"methodSignatureForSelector:","missingFixtureSelector:"));
    assert(!method_signature(target,nullptr,nullptr));assert(!method_signature(nullptr,nullptr,"pausePressed:"));
    auto* subclass=static_cast<Class*>(rt_class("NXInvocationSubclass"));subclass->super=static_cast<Class*>(rt_class("GameLayer"));subclass->isa->super=subclass->super->isa;initialized.insert(subclass);
    Obj derived=rt_alloc(subclass);assert(!strcmp(rt_method_types(derived,"pausePressed:"),"v24@0:8@16"));
    Obj class_signature=send<Obj>(rt_class("GameLayer"),"methodSignatureForSelector:","node");assert(class_signature&&rt_host(class_signature).text==rt_method_types(rt_class("GameLayer"),"node"));
    Obj signature=send<Obj>(derived,"methodSignatureForSelector:","pausePressed:");
    Obj kept=send<Obj>(rt_class("NSInvocation"),"invocationWithMethodSignature:",signature);hold(kept);
    send<void>(kept,"setTarget:",derived);send<void>(kept,"setSelector:","pausePressed:");
    Obj argument=sender;send<void>(kept,"setArgument:atIndex:",static_cast<const void*>(&argument),uint64_t(2));
    auto pending=std::move(autoreleases);autoreleases.clear();for(Obj object:pending)ns_release(object,nullptr);
    assert(invocations.size()==1&&host.count(signature)&&rt_host(signature).references==1);
    send<void>(kept,"invoke");assert(observed_target==derived&&observed_sender==sender&&calls==8);
    send<void>(kept,"setTarget:",static_cast<Obj>(nullptr));send<void>(kept,"invoke");assert(calls==8);
    drop(kept);assert(invocations.empty()&&!host.count(signature));assert(rt_host(derived).references==1&&rt_host(sender).references==1);
    pid_t child=fork();assert(child>=0);if(!child){Obj unsupported=method_signature(rt_class("GameLayer"),nullptr,"node");invocation_create(nullptr,nullptr,unsupported);_exit(1);}
    int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==90);
    for(Obj object:{target,derived,sender,other})ns_dealloc(object,nullptr);
    puts("{\"status\":\"PASS\",\"original_GameLayer_encodings\":7,\"signature_from_metadata\":true,\"instance_class_super_lookup\":true,\"missing_nil\":true,\"typed_host_callbacks\":8,\"argument_bytes_copied\":true,\"target_sender_not_retained\":true,\"signature_retained_and_released\":true,\"nil_target_noop\":true,\"unsupported_shape_rejected\":true,\"executes_guest_code\":false}");
}
