// Targeted MainTask/NSBlock tests; includes stubs, never runs the Mach-O guest.
#define main existing_runtime_fixture_main
#include "host_runtime_fixture.cpp"
#undef main
#include <sanitizer/lsan_interface.h>
static Obj test_main_thread;
static std::vector<int> events;
static int copy_calls=0,dispose_calls=0;
static Obj main_thread_object(Obj,Sel){return test_main_thread;}
static void record_first(Obj target,Sel,Obj arg){
    assert(host.count(target));if(arg)assert(host.count(arg));events.push_back(1);
}
static void record_last(Obj target,Sel,Obj arg){
    assert(host.count(target));if(arg)assert(host.count(arg));events.push_back(3);
}
struct LifetimeBlock {Block b;Obj argument;int event;};
static void lifetime_invoke(void*p){auto b=static_cast<LifetimeBlock*>(p);if(b->argument)assert(host.count(b->argument));events.push_back(b->event);}
static void lifetime_copy(void*dst,void*src){++copy_calls;block_assign(&static_cast<LifetimeBlock*>(dst)->argument,static_cast<LifetimeBlock*>(src)->argument,3);}
static void lifetime_dispose(void*p){++dispose_calls;block_dispose(static_cast<LifetimeBlock*>(p)->argument,3);}
static BlockDesc lifetime_desc{0,sizeof(LifetimeBlock),lifetime_copy,lifetime_dispose};
static LifetimeBlock make_block(Obj arg,int event){return {{rt_class("NSBlock"),1u<<25,0,lifetime_invoke,&lifetime_desc},arg,event};}
static void objc_blocks_test(){
    Obj arg=rt_new("NSObject");auto stack=make_block(arg,7);
    assert(send<Obj>(&stack,"retain")==&stack&&block_refs.empty());
    send<void>(&stack,"release");assert(dispose_calls==0);
    Obj heap=send<Obj>(&stack,"copy");assert(heap!=&stack&&copy_calls==1&&host[arg].references==2);
    assert(send<Obj>(heap,"retain")==heap&&block_refs.at(heap)==2);
    assert(send<Obj>(heap,"copyWithZone:",Obj(nullptr))==heap&&block_refs.at(heap)==3&&copy_calls==1);
    // The generic retain/release route must detect NSBlock as well.
    assert(ns_retain(heap,nullptr)==heap&&block_refs.at(heap)==4);
    ns_release(heap,nullptr);send<void>(heap,"release");send<void>(heap,"release");
    assert(block_refs.at(heap)==1&&dispose_calls==0);
    Obj pool=rt_new("NSAutoreleasePool");assert(send<Obj>(heap,"autorelease")==heap);
    send<void>(pool,"drain");assert(block_refs.empty()&&dispose_calls==1&&host[arg].references==1);
    ns_release(arg,nullptr);assert(!host.count(arg));
    auto global=make_block(nullptr,8);global.b.flags=1u<<28;
    assert(send<Obj>(&global,"copy")==&global&&send<Obj>(&global,"retain")==&global);
    send<void>(&global,"release");assert(block_refs.empty()&&dispose_calls==1);
}
static void fifo_lifetime_test(){
    events.clear();Obj target=rt_new("QueueFixture"),arg=rt_new("NSObject");auto block=make_block(arg,2);
    send<void>(target,"performSelector:onThread:withObject:waitUntilDone:",Sel("first:"),test_main_thread,arg,false);
    async_main(main_dispatch_queue.get(),&block);
    send<void>(target,"performSelectorOnMainThread:withObject:waitUntilDone:",Sel("last:"),arg,false);
    assert(events.empty()&&host[target].references==3&&host[arg].references==4);
    ns_release(target,nullptr);ns_release(arg,nullptr);
    rt_drain_main_queue();
    assert((events==std::vector<int>{1,2,3})&&!host.count(target)&&!host.count(arg)&&block_refs.empty()&&main_queue.empty());
}
static void enqueue_during_callback(Obj target,Sel,Obj arg){
    events.push_back(4);
    send<void>(target,"performSelectorOnMainThread:withObject:waitUntilDone:",Sel("last:"),arg,false);
}
static void deferred_and_sync_test(){
    events.clear();Obj target=rt_new("QueueFixture");
    send<void>(target,"performSelectorOnMainThread:withObject:waitUntilDone:",Sel("enqueue:"),Obj(nullptr),false);
    send<void>(target,"performSelectorOnMainThread:withObject:waitUntilDone:",Sel("first:"),Obj(nullptr),false);
    unsigned refs=host[target].references;
    send<void>(target,"performSelectorOnMainThread:withObject:waitUntilDone:",Sel("last:"),Obj(nullptr),true);
    assert((events==std::vector<int>{3})&&host[target].references==static_cast<int>(refs));
    ns_release(target,nullptr);
    rt_drain_main_queue();assert((events==std::vector<int>{3,4,1})&&host.count(target)&&main_queue.size()==1);
    rt_drain_main_queue();assert((events==std::vector<int>{3,4,1,3})&&!host.count(target)&&main_queue.empty());
}
static void rejection_tests(){
    Obj target=rt_new("QueueFixture");
    pid_t child=fork();assert(child>=0);
    if(!child){std::thread worker([&](){ns_perform_on_main(target,nullptr,"first:",nullptr,true);});worker.join();_exit(1);}
    int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==90);
    child=fork();assert(child>=0);
    if(!child){ns_perform_on_thread(target,nullptr,"first:",target,nullptr,false);_exit(1);}
    assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==90);
    ns_release(target,nullptr);
}
int main(){
    rt_install_base();test_main_thread=rt_new("NSThread");
    rt_method("NSThread","mainThread",reinterpret_cast<void*>(main_thread_object),true);
    rt_method("QueueFixture","first:",reinterpret_cast<void*>(record_first));
    rt_method("QueueFixture","last:",reinterpret_cast<void*>(record_last));
    rt_method("QueueFixture","enqueue:",reinterpret_cast<void*>(enqueue_during_callback));
    // Host class/metaclass descriptors intentionally live for the process.
    // Exclude only those descriptors; test instances/Blocks remain LSan-tracked.
    for(auto c:class_objects){__lsan_ignore_object(c);__lsan_ignore_object(ro(static_cast<Class*>(c)));}
    objc_blocks_test();fifo_lifetime_test();deferred_and_sync_test();rejection_tests();
    ns_release(test_main_thread,nullptr);
    assert(main_queue.empty()&&block_refs.empty()&&autoreleases.empty());
    puts("PASS: NSBlock ObjC copy/retain/release/autorelease and global/stack identity; mixed selector/Block FIFO; target/argument lifetime after caller releases; deferred enqueue during drain; wait=true main inline; unsupported thread/sync rejection; no ARM64 guest execution");
}
