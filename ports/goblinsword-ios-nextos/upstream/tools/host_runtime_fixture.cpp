#include "runtime.cpp"
#include <cassert>
#include <atomic>
#include <chrono>
#include <sys/wait.h>
extern "C" void objc_msgSend(){}
extern "C" void objc_msgSendSuper2(){}
extern "C" void rt_nil(){}
extern "C" void rt_printf_bridge(){}
extern "C" void rt_nslog_bridge(){}
static int cleanup_count=0;
void rt_foundation_dealloc(Obj){++cleanup_count;}
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
static uintptr_t old_method(Obj,Sel){return 10;}
static uintptr_t new_method(Obj,Sel){return 20;}
static std::atomic<int> active{0};
static std::mutex test_mutex;
static std::condition_variable test_cv;
static bool test_entered=false,test_release=false;
static std::vector<int> order;
struct TestBlock { Block b; int id; };
static void serial_test(void*p){auto*b=static_cast<TestBlock*>(p);assert(active.fetch_add(1)==0);order.push_back(b->id);if(!b->id){std::unique_lock<std::mutex>lock(test_mutex);test_entered=true;test_cv.notify_all();test_cv.wait(lock,[](){return test_release;});}assert(active.fetch_sub(1)==1);}
static void main_test(void*p){order.push_back(static_cast<TestBlock*>(p)->id);}
static void* recursive_queue=nullptr;
static void recursive_test(void*p){dispatch_sync_queue(recursive_queue,p);}
static void queues_test(){
 void*q=dispatch_create("fixture.serial",nullptr);auto data=dispatch_find(q);
 BlockDesc desc{0,sizeof(TestBlock),nullptr,nullptr};TestBlock b0{{nullptr,0,0,serial_test,&desc},0},b1{{nullptr,0,0,serial_test,&desc},1},b2{{nullptr,0,0,serial_test,&desc},2};
 std::thread t0([&](){dispatch_sync_queue(q,&b0);});{std::unique_lock<std::mutex>lock(test_mutex);test_cv.wait(lock,[](){return test_entered;});}
 auto issued=[&](unsigned goal){for(unsigned i=0;i<1000;++i){{std::lock_guard<std::mutex>guard(data->state);if(data->issued>=goal)return;}std::this_thread::sleep_for(std::chrono::milliseconds(1));}assert(false);};
 std::thread t1([&](){dispatch_sync_queue(q,&b1);});issued(2);std::thread t2([&](){dispatch_sync_queue(q,&b2);});issued(3);
 {std::lock_guard<std::mutex>guard(test_mutex);test_release=true;test_cv.notify_all();}t0.join();t1.join();t2.join();assert((order==std::vector<int>{0,1,2}));
 TestBlock bm{{nullptr,0,0,main_test,&desc},3};async_main(main_dispatch_queue.get(),&bm);assert(order.size()==3);rt_drain_main_queue();assert(order.size()==4&&order.back()==3&&block_refs.empty());
 pid_t child=fork();assert(child>=0);if(!child){async_main(q,&bm);_exit(1);}int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==90);
 child=fork();assert(child>=0);if(!child){recursive_queue=q;TestBlock br{{nullptr,0,0,recursive_test,&desc},4};dispatch_sync_queue(q,&br);_exit(1);}assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==90);
 dispatch_release_queue(q);assert(!dispatch_queues.count(q));
}
int main(int argc,char**argv){
 assert(argc==2);rt_install_base();
 assert(rt_resolve("_OBJC_CLASS_$_NSMutableArray",false,nullptr)==rt_class("NSMutableArray"));
 assert(rt_resolve("_OBJC_METACLASS_$_NSMutableArray",false,nullptr)==static_cast<Class*>(rt_class("NSMutableArray"))->isa);
 Obj array=rt_new("NSMutableArray");assert(ns_kind(array,nullptr,rt_class("NSArray")));
 uint64_t formatargs[]={reinterpret_cast<uintptr_t>(array),0};assert(format_stack("%@ %@",formatargs)=="NSMutableArray (null)");
 MachImage image;std::string error;
 assert(macho_load(argv[1],resolver,nullptr,image,error));rt_register_image(image);assert(protocols.size()==30);
 auto* director=static_cast<Class*>(rt_class("CCDirector"));assert(lookup_chain(director->isa,"isEqual:",true)==reinterpret_cast<void*>(ns_equal));assert(ns_equal(director,nullptr,director));
 queues_test();
 auto*p=static_cast<Protocol*>(protocol_get("__ARCLiteKeyedSubscripting__"));assert(p);
 auto d=protocol_description(p,"setObject:forKeyedSubscript:",true,true);
 assert(d.name&&!strcmp(d.name,"setObject:forKeyedSubscript:")&&d.types&&!strcmp(d.types,"v32@0:8@16@24"));
 auto missing=protocol_description(p,"missingTestSelector",true,true);assert(!missing.name&&!missing.types);
 auto wrong=protocol_description(p,"setObject:forKeyedSubscript:",true,false);assert(!wrong.name&&!wrong.types);
 auto*cls=static_cast<Class*>(rt_class("NXReflectionFixture"));
 assert(class_add(cls,"value",reinterpret_cast<void*>(old_method),"Q16@0:8"));
 assert(!class_add(cls,"value",reinterpret_cast<void*>(new_method),"Q16@0:8"));
 Method*m=class_instance_method(cls,"value");assert(m&&m->imp==reinterpret_cast<void*>(old_method));
 assert(method_replace(m,reinterpret_cast<void*>(new_method))==reinterpret_cast<void*>(old_method));
 Obj o=rt_new("NXReflectionFixture");assert(send<uintptr_t>(o,"value")==20);
 assert(class_replace(cls,"value",reinterpret_cast<void*>(old_method),"Q16@0:8")==reinterpret_cast<void*>(new_method));assert(send<uintptr_t>(o,"value")==10);
 Class compiled{},meta{};ClassRO cr{},mr{};auto*base=static_cast<Class*>(rt_class("NSObject"));
 cr.name="NXCompiledClassFixture";cr.start=8;cr.size=16;mr.name=cr.name;mr.flags=1;mr.size=40;
 compiled.isa=&meta;compiled.super=base;compiled.bits=reinterpret_cast<uintptr_t>(&cr);meta.isa=base->isa;meta.super=base->isa;meta.bits=reinterpret_cast<uintptr_t>(&mr);
 assert(read_class_pair(&compiled,nullptr)==&compiled);assert(objc_getclass(cr.name)==&compiled);assert(read_class_pair(&compiled,nullptr)==nullptr);
 Obj app=application_shared(nullptr,nullptr);assert(app==application_shared(nullptr,nullptr));application_set_delegate(app,nullptr,o);assert(application_delegate(app,nullptr)==o);
 ns_dealloc(o,nullptr);assert(cleanup_count==1&&!host.count(o));
 puts("PASS: 30 original protocols; ARCLite required-instance signature; missing/class-method rejection; class_add/replace/swizzle affect dispatch and preserve old IMP; compiled class registration and duplicate rejection; UIApplication singleton/delegate; Foundation cleanup hook; no ARM64 guest execution");
 return 0;
}
