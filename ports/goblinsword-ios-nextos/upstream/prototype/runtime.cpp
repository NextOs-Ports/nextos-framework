#include "runtime.h"
#include "macho_loader.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <algorithm>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <dlfcn.h>
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>

struct Method { const char* name; const char* types; void* imp; };
struct Methods { uint32_t size, count; Method entries[1]; };
struct ClassRO {
    uint32_t flags, start, size, reserved;
    void* ivarlayout; const char* name; Methods* methods;
    void* protocols; void* ivars; void* weaklayout; void* properties;
};
struct Class { Class* isa; Class* super; uintptr_t cache[2]; uintptr_t bits; };
struct ProtocolList { uintptr_t count; void* entries[1]; };
struct Protocol {
    void* isa; const char* name; ProtocolList* parents;
    Methods *required_instance, *required_class, *optional_instance, *optional_class;
    void* properties; uint32_t size, flags;
};
struct MethodDescription { Sel name; const char* types; };
MachImage* rt_image = nullptr;
std::string rt_data_root;
static std::unordered_map<std::string, Class*> classes;
static std::unordered_set<Class*> class_objects;
static std::unordered_map<Obj,HostData> host;
static std::unordered_map<std::string,void*> overrides;
static std::unordered_map<std::string,Protocol*> protocols;
static ProtocolList nsobject_protocols{};
static std::unordered_map<std::string,std::unique_ptr<Method>> reflected_overrides;
static std::unordered_map<Method*,std::string> reflected_keys;
static Obj application_singleton = nullptr;
static std::unordered_set<Class*> initializing, initialized;
static std::vector<Obj> autoreleases;
static std::unordered_map<std::string,void*> traps, constants;
static std::unordered_map<void*,std::unique_ptr<std::recursive_mutex>> mutexes;
static ClassRO* ro(Class* c) { return c ? reinterpret_cast<ClassRO*>(c->bits & ~uintptr_t(7)) : nullptr; }
static bool is_class(Obj p) { return class_objects.count(static_cast<Class*>(p)); }
static std::string method_key(const char* name,const char* sel,bool cls) { return std::string(cls?"+":"-")+name+" "+sel; }
extern "C" void rt_nil();
extern "C" void rt_printf_bridge();
extern "C" void rt_nslog_bridge();
extern void rt_graphics_run_loop();
extern void* rt_darwin_symbol(const char*);
static Obj ns_block_copy(Obj,Sel,void*);
static Obj ns_block_retain(Obj,Sel);
static void ns_block_release(Obj,Sel);
static void ns_perform_on_thread(Obj,Sel,Sel,Obj,Obj,bool);
static void ns_perform_on_main(Obj,Sel,Sel,Obj,bool);
extern void rt_foundation_dealloc(Obj);

[[noreturn]] void rt_fail(const char* reason) {
    fprintf(stderr,"FATAL %s\n",reason); fflush(stderr); exit(90);
}
static void* own_method(Class* c,const char* selector) {
    if (!c || !ro(c) || !ro(c)->methods) return nullptr;
    Methods* m=ro(c)->methods;
    if ((m->size & 0xffff) < 24 || (m->size & 0x80000000)) rt_fail("unsupported Objective-C method format");
    for(uint32_t i=0;i<m->count;++i) {
        auto p=reinterpret_cast<Method*>(reinterpret_cast<char*>(m)+8+(m->size&0xffff)*i);
        if(p->name && !strcmp(p->name,selector)) return p->imp;
    }
    return nullptr;
}
static void* lookup_chain(Class* c,const char* selector,bool class_method) {
    (void)class_method;
    for(unsigned depth=0;c && depth<100;++depth,c=c->super) {
        auto r=ro(c); if(!r||!r->name) rt_fail("invalid class metadata");
        // The root metaclass inherits from the root class itself. At that final
        // hop NSObject instance methods also service class receivers (isEqual:,
        // hash, retain, ...). Use each node's real metadata kind, not the kind
        // of the initial receiver for every node in the chain.
        auto it=overrides.find(method_key(r->name,selector,(r->flags&1)!=0));
        if(it!=overrides.end()) return it->second;
        if(void* imp=own_method(c,selector)) return imp;
    }
    return nullptr;
}
static void initialize(Class* c) {
    if(!c || initialized.count(c) || initializing.count(c)) return;
    initializing.insert(c); initialize(c->super);
    if(void* imp=lookup_chain(c->isa,"initialize",true))
        reinterpret_cast<void(*)(Obj,Sel)>(imp)(c,"initialize");
    initialized.insert(c); initializing.erase(c);
}
extern "C" void* rt_lookup(Obj obj,Sel selector) {
    if(!obj) return reinterpret_cast<void*>(rt_nil);
    if(!selector) rt_fail("NULL selector");
    bool cls=is_class(obj);
    Class* c=cls?static_cast<Class*>(obj):*static_cast<Class**>(obj);
    if(!c) rt_fail("object with NULL isa");
    if(strcmp(selector,"initialize")) initialize(c);
    if(void* imp=lookup_chain(cls?c->isa:c,selector,cls)) return imp;
    fprintf(stderr,"MISSING_METHOD %c[%s %s] receiver=%p caller=%p\n",cls?'+':'-',ro(c)->name,selector,obj,__builtin_return_address(0));
    rt_fail("Objective-C method not implemented");
}
extern "C" void* rt_lookup_super(void* p,Sel selector) {
    auto pair=static_cast<Obj*>(p);
    Class* current=static_cast<Class*>(pair[1]);
    bool cls=is_class(pair[0]);
    if(void* imp=lookup_chain(current->super,selector,cls)) return imp;
    fprintf(stderr,"MISSING_SUPER %s %s\n",ro(current)->name,selector);
    rt_fail("Objective-C superclass method not implemented");
}
bool rt_responds(Obj obj,Sel sel) {
    if(!obj) return false;
    bool cls=is_class(obj); Class* c=cls?static_cast<Class*>(obj):*static_cast<Class**>(obj);
    return lookup_chain(cls?c->isa:c,sel,cls)!=nullptr;
}
void* rt_class(const char* name) {
    auto it=classes.find(name); if(it!=classes.end()) return it->second;
    auto c=new Class{}; auto meta=new Class{};
    auto cr=new ClassRO{}; auto mr=new ClassRO{};
    cr->name=strdup(name); cr->size=8; mr->name=cr->name; mr->size=40; mr->flags=1;
    c->isa=meta;c->bits=reinterpret_cast<uintptr_t>(cr);meta->bits=reinterpret_cast<uintptr_t>(mr);
    classes[name]=c;class_objects.insert(c);class_objects.insert(meta);
    if(strcmp(name,"NSObject")) {
        const char* parent="NSObject";
        static const std::unordered_map<std::string,const char*> host_parents={
            {"NSMutableString","NSString"},{"NSMutableArray","NSArray"},
            {"NSMutableDictionary","NSDictionary"},{"NSMutableSet","NSSet"},
            {"NSMutableData","NSData"}
        };
        auto pi=host_parents.find(name);if(pi!=host_parents.end())parent=pi->second;
        c->super=static_cast<Class*>(rt_class(parent));meta->super=c->super->isa;
        auto root=c->super;while(root->super)root=root->super;meta->isa=root->isa;
    }
    else {meta->isa=meta;meta->super=c;}
    return c;
}
const char* rt_class_name(Obj obj) {
    if(!obj)return "nil";
    Class* c=is_class(obj)?static_cast<Class*>(obj):*static_cast<Class**>(obj);
    return ro(c)->name;
}
HostData& rt_host(Obj obj) {return host[obj];}
Obj rt_alloc(Obj cls) {
    auto c=static_cast<Class*>(cls);
    size_t size=std::max<size_t>(ro(c)->size,8);
    if(size>16*1024*1024) rt_fail("invalid Objective-C instance size");
    auto p=calloc(1,size);if(!p)rt_fail("allocation failed");
    *static_cast<Class**>(p)=c;host[p]=HostData{};return p;
}
Obj rt_new(const char* classname) {return rt_alloc(rt_class(classname));}
Obj rt_string(const char* value) {Obj p=rt_new("NSString");host[p].text=value?value:"";return p;}
const char* rt_utf8(Obj obj) {
    if(!obj)return "";
    auto it=host.find(obj);if(it!=host.end())return it->second.text.c_str();
    // NSConstantString uses the public CF constant layout; convert UTF16 if needed.
    const char* name=rt_class_name(obj);
    if(!strcmp(name,"NSConstantString") || !strcmp(name,"__NSCFConstantString")) {
        uint32_t flags=*reinterpret_cast<uint32_t*>(static_cast<char*>(obj)+8);
        auto str=*reinterpret_cast<const char**>(static_cast<char*>(obj)+16);
        uint64_t len=*reinterpret_cast<uint64_t*>(static_cast<char*>(obj)+24);
        if(len>1024*1024)rt_fail("constant string length too large");
        auto& h=host[obj];h.references=0x3fffffff;
        if(flags&0x10) {
            auto u=reinterpret_cast<const uint16_t*>(str);
            for(uint64_t i=0;i<len;++i){unsigned v=u[i];if(v<128)h.text+=char(v);else if(v<2048){h.text+=char(0xc0|(v>>6));h.text+=char(0x80|(v&63));}else{h.text+=char(0xe0|(v>>12));h.text+=char(0x80|((v>>6)&63));h.text+=char(0x80|(v&63));}}
        } else h.text.assign(str,len);
        return h.text.c_str();
    }
    fprintf(stderr,"UTF8 requested from %s\n",name);rt_fail("unsupported string object");
}
void rt_method(const char* classname,const char* selector,void* imp,bool cls) {
    rt_class(classname);auto key=method_key(classname,selector,cls);overrides[key]=imp;
    auto i=reflected_overrides.find(key);if(i!=reflected_overrides.end())i->second->imp=imp;
}
static Obj ns_alloc(Obj cls,Sel,...){return rt_alloc(cls);}
static Obj ns_init(Obj o,Sel){return o;}
static Obj ns_new(Obj cls,Sel){return send<Obj>(rt_alloc(cls),"init");}
static Obj ns_retain(Obj o,Sel){if(o&&!strcmp(rt_class_name(o),"NSBlock"))return ns_block_retain(o,nullptr);auto i=host.find(o);if(i!=host.end()&&i->second.references<0x3fffffff)++i->second.references;return o;}
static void ns_dealloc(Obj o,Sel){rt_foundation_dealloc(o);host.erase(o);free(o);}
static void ns_release(Obj o,Sel){if(o&&!strcmp(rt_class_name(o),"NSBlock")){rt_block_release(o);return;}auto i=host.find(o);if(i==host.end()||i->second.references>=0x3fffffff)return;if(--i->second.references==0)send<void>(o,"dealloc");}
static Obj ns_autorelease(Obj o,Sel){autoreleases.push_back(o);return o;}
static Obj ns_class(Obj o,Sel){return is_class(o)?o:*static_cast<Obj*>(o);}
static Obj ns_super(Obj o,Sel){return static_cast<Class*>(ns_class(o,nullptr))->super;}
static bool ns_respond(Obj o,Sel,Sel s){return rt_responds(o,s);}
static bool ns_kind(Obj o,Sel,Obj wanted){for(auto c=static_cast<Class*>(ns_class(o,nullptr));c;c=c->super)if(c==wanted)return true;return false;}
static bool ns_member(Obj o,Sel,Obj wanted){return ns_class(o,nullptr)==wanted;}
static bool ns_equal(Obj o,Sel,Obj other){return o==other;}
static uint64_t ns_hash(Obj o,Sel){return reinterpret_cast<uintptr_t>(o);}
static Obj ns_description(Obj o,Sel){return rt_string(rt_class_name(o));}
static void ns_initialize(Obj,Sel){}
static Obj ns_copy(Obj o,Sel,void*){return send<Obj>(o,"copyWithZone:",Obj(nullptr));}
static Obj ns_perform(Obj o,Sel,Sel action,Obj arg){return send<Obj>(o,action,arg);}
static void pool_drain(Obj o,Sel){auto list=std::move(autoreleases);autoreleases.clear();for(auto p:list)ns_release(p,nullptr);ns_release(o,nullptr);}
static Obj application_shared(Obj,Sel){if(!application_singleton)application_singleton=rt_new("UIApplication");return application_singleton;}
static Obj application_delegate(Obj o,Sel){return rt_host(o).target;}
// The supplied bundle declares UIStatusBarHidden=true and landscape support.
static Rect application_status_bar(Obj,Sel){return {{0,0},{0,0}};}
static bool application_status_hidden(Obj,Sel){return true;}
static long application_orientation(Obj,Sel){return 3;}
static void application_set_delegate(Obj o,Sel,Obj delegate){rt_host(o).target=delegate;}

static bool image_range(const void* pointer,size_t size) {
    if(!rt_image)return false;
    uintptr_t p=reinterpret_cast<uintptr_t>(pointer);
    for(const auto&s:rt_image->segments) {
        uintptr_t start=reinterpret_cast<uintptr_t>(s.address);
        if(s.address&&p>=start&&p-start<=s.vmsize&&size<=s.vmsize-(p-start))return true;
    }
    return false;
}
static void register_protocol(Protocol* p,unsigned depth=0) {
    if(!p)return;
    if(depth>64||!image_range(p,sizeof(Protocol))||p->size<sizeof(Protocol)||!image_range(p->name,1))rt_fail("invalid protocol metadata");
    if(protocols.count(p->name))return;
    protocols[p->name]=p;p->isa=rt_class("Protocol");
    if(p->parents){if(!image_range(p->parents,8)||p->parents->count>4096||!image_range(p->parents,8+8*p->parents->count))rt_fail("invalid protocol inheritance list");
        for(uintptr_t i=0;i<p->parents->count;++i)register_protocol(static_cast<Protocol*>(p->parents->entries[i]),depth+1);}
}
static Method* list_method(Methods* list,Sel selector) {
    if(!list||!selector)return nullptr;
    uint32_t stride=list->size&0xffff;
    if(stride<24||(list->size&0x80000000)||list->count>100000)rt_fail("invalid reflection method list");
    for(uint32_t i=0;i<list->count;++i){auto m=reinterpret_cast<Method*>(reinterpret_cast<char*>(list)+8+uint64_t(i)*stride);
        if(m->name&&!strcmp(m->name,selector))return m;}
    return nullptr;
}
static MethodDescription protocol_description_inner(Protocol*p,Sel selector,bool required,bool instance,std::unordered_set<Protocol*>&seen) {
    if(!p||!seen.insert(p).second)return {nullptr,nullptr};
    if(seen.size()>4096)rt_fail("protocol inheritance limit");
    Methods* list=required?(instance?p->required_instance:p->required_class):(instance?p->optional_instance:p->optional_class);
    if(list){if(!image_range(list,8)||!image_range(list,8+uint64_t(list->size&0xffff)*list->count))rt_fail("protocol methods outside image");
        if(Method*m=list_method(list,selector))return {m->name,m->types};}
    if(p->parents)for(uintptr_t i=0;i<p->parents->count;++i){auto result=protocol_description_inner(static_cast<Protocol*>(p->parents->entries[i]),selector,required,instance,seen);if(result.name)return result;}
    return {nullptr,nullptr};
}
static MethodDescription protocol_description(Protocol*p,Sel selector,bool required,bool instance) {
    if(!p||!selector)return {nullptr,nullptr};
    register_protocol(p);
    std::unordered_set<Protocol*> seen;
    return protocol_description_inner(p,selector,required,instance,seen);
}
static Obj protocol_get(const char*name){if(!name)return nullptr;auto i=protocols.find(name);return i==protocols.end()?nullptr:i->second;}
static const char* protocol_name(Protocol*p){return p?p->name:nullptr;}
static void validate_protocol_list(ProtocolList* list){
    if(list==&nsobject_protocols)return;
    if(!image_range(list,sizeof(uintptr_t))||list->count>4096||!image_range(list,sizeof(uintptr_t)*(1+list->count)))rt_fail("protocol list outside image");
}
static bool protocol_conforms(Protocol* candidate,Protocol* wanted,std::unordered_set<Protocol*>& seen){
    if(!candidate||!seen.insert(candidate).second)return false;
    if(seen.size()>4096)rt_fail("protocol inheritance limit");
    register_protocol(candidate);
    if(candidate==wanted||!std::strcmp(candidate->name,wanted->name))return true;
    if(candidate->parents){validate_protocol_list(candidate->parents);for(uintptr_t i=0;i<candidate->parents->count;++i)
        if(protocol_conforms(static_cast<Protocol*>(candidate->parents->entries[i]),wanted,seen))return true;}
    return false;
}
static bool ns_conforms(Obj object,Sel,Protocol* wanted){
    if(!object||!wanted)return false;
    register_protocol(wanted);
    std::unordered_set<Protocol*> seen_protocols;std::unordered_set<Class*> seen_classes;
    // For a class receiver query that class's declared instance conformance,
    // not the metaclass's list. Both forms include adopted protocols of bases.
    for(auto c=static_cast<Class*>(ns_class(object,nullptr));c;c=c->super){
        if(!seen_classes.insert(c).second||seen_classes.size()>100)rt_fail("invalid class inheritance in protocol query");
        if(!ro(c))rt_fail("invalid class metadata in protocol query");
        auto list=static_cast<ProtocolList*>(ro(c)->protocols);if(!list)continue;
        validate_protocol_list(list);
        for(uintptr_t i=0;i<list->count;++i)if(protocol_conforms(static_cast<Protocol*>(list->entries[i]),wanted,seen_protocols))return true;
    }
    return false;
}
static bool class_meta(Class*c){return c&&ro(c)&&(ro(c)->flags&1);}
static Method* class_own_reflected_method(Class*c,Sel selector) {
    if(!c||!selector)return nullptr;
    auto key=method_key(ro(c)->name,selector,class_meta(c));
    auto i=overrides.find(key);
    if(i!=overrides.end()){
        auto& holder=reflected_overrides[key];
        if(!holder){Method* original=list_method(ro(c)->methods,selector);holder=std::make_unique<Method>(Method{strdup(selector),original?original->types:nullptr,i->second});reflected_keys[holder.get()]=key;}
        holder->imp=i->second;return holder.get();
    }
    return list_method(ro(c)->methods,selector);
}
static Method* class_instance_method(Class*c,Sel selector){for(unsigned depth=0;c&&depth<100;++depth,c=c->super)if(auto*m=class_own_reflected_method(c,selector))return m;return nullptr;}
const char* rt_method_types(Obj object,Sel selector){
    if(!object||!selector)return nullptr;
    bool cls=is_class(object);auto c=cls?static_cast<Class*>(object):*static_cast<Class**>(object);
    if(auto* method=class_instance_method(cls?c->isa:c,selector))return method->types;
    return nullptr;
}
static void* ns_instance_method_for_selector(Obj cls,Sel,Sel selector){
    if(auto*m=class_instance_method(static_cast<Class*>(cls),selector))return m->imp;
    std::fprintf(stderr,"MISSING_IMP -[%s %s]\n",ro(static_cast<Class*>(cls))->name,selector);
    rt_fail("instanceMethodForSelector: has no implementation");
}
static void* ns_method_for_selector(Obj o,Sel,Sel selector){return rt_lookup(o,selector);}
static bool ns_instances_respond(Obj cls,Sel,Sel selector){return class_instance_method(static_cast<Class*>(cls),selector)!=nullptr;}
static void* method_replace(Method*m,void*imp){if(!m||!imp)return nullptr;void*old=m->imp;m->imp=imp;auto i=reflected_keys.find(m);if(i!=reflected_keys.end())overrides[i->second]=imp;return old;}
static void* class_replace(Class*c,Sel selector,void*imp,const char*types) {
    if(!c||!selector||!imp)return nullptr;
    Method* old=class_own_reflected_method(c,selector);void*previous=old?old->imp:nullptr;
    auto key=method_key(ro(c)->name,selector,class_meta(c));
    auto& holder=reflected_overrides[key];
    if(!holder){holder=std::make_unique<Method>(Method{strdup(selector),types?strdup(types):nullptr,imp});reflected_keys[holder.get()]=key;}
    else {holder->imp=imp;if(types)holder->types=strdup(types);}
    overrides[key]=imp;
    std::fprintf(stderr,"OBJC_REPLACE %c[%s %s] old=%p new=%p\n",class_meta(c)?'+':'-',ro(c)->name,selector,previous,imp);
    return previous;
}
static bool class_add(Class*c,Sel selector,void*imp,const char*types){if(!c||!selector||!imp||class_own_reflected_method(c,selector))return false;class_replace(c,selector,imp,types);return true;}
static Obj read_class_pair(Class*c,const void*) {
    // Read the compiler's actual class/metaclass image. This API does not invoke
    // +load or +initialize. Unsupported ivar sliding fails instead of pretending
    // that a Darwin realization step took place.
    if(!c||!c->isa||!ro(c)||!ro(c->isa)||!ro(c)->name)return nullptr;
    if(classes.count(ro(c)->name))return nullptr;
    if(!c->super&&!(ro(c)->flags&2))return nullptr;
    if(c->super&&(!class_objects.count(c->super)||ro(c)->start<ro(c->super)->size))return nullptr;
    if(ro(c)->size<ro(c)->start||ro(c)->size>16*1024*1024||!(ro(c->isa)->flags&1))return nullptr;
    classes[ro(c)->name]=c;class_objects.insert(c);class_objects.insert(c->isa);
    return c;
}

void rt_register_image(MachImage& image) {
    rt_image=&image;
    auto s=image.find_section("__DATA","__objc_classlist");
    if(!s)rt_fail("missing classlist");
    auto arr=static_cast<Class**>(s->address);
    for(size_t i=0;i<s->size/8;++i){Class*c=arr[i];classes[ro(c)->name]=c;class_objects.insert(c);class_objects.insert(c->isa);}
    if(auto p=image.find_section("__DATA","__objc_protolist"))for(size_t i=0;i<p->size/8;++i)register_protocol(static_cast<Protocol**>(p->address)[i]);
    // The host NSObject implementation adopts NSObject just as its declaration
    // does. Use the image's registered protocol; do not invent protocol data.
    auto* root=static_cast<Class*>(rt_class("NSObject"));
    if(auto p=protocol_get("NSObject");p&&!image_range(root,sizeof(Class))){nsobject_protocols.count=1;nsobject_protocols.entries[0]=p;ro(root)->protocols=&nsobject_protocols;}
    fprintf(stderr,"REGISTERED_CLASSES %zu\n",s->size/8);
    auto cat=image.find_section("__DATA","__objc_catlist");
    if(cat) for(size_t i=0;i<cat->size/8;++i) {
        auto p=static_cast<uintptr_t**>(cat->address)[i];auto c=reinterpret_cast<Class*>(p[1]);
        for(int kind=0;kind<2;++kind){auto ml=reinterpret_cast<Methods*>(p[2+kind]);if(!ml)continue;
            for(uint32_t j=0;j<ml->count;++j){auto m=reinterpret_cast<Method*>(reinterpret_cast<char*>(ml)+8+j*(ml->size&0xffff));rt_method(ro(c)->name,m->name,m->imp,kind);}}
    }
}
void rt_install_base() {
#define M(s,f) rt_method("NSObject",s,reinterpret_cast<void*>(f))
    M("init",ns_init);M("retain",ns_retain);M("release",ns_release);M("autorelease",ns_autorelease);M("dealloc",ns_dealloc);
    M("class",ns_class);M("superclass",ns_super);M("respondsToSelector:",ns_respond);M("isKindOfClass:",ns_kind);M("isMemberOfClass:",ns_member);M("isEqual:",ns_equal);M("hash",ns_hash);M("description",ns_description);M("copy",ns_copy);M("performSelector:",ns_perform);M("performSelector:withObject:",ns_perform);
    M("performSelector:onThread:withObject:waitUntilDone:",ns_perform_on_thread);
    M("performSelectorOnMainThread:withObject:waitUntilDone:",ns_perform_on_main);
    M("methodForSelector:",ns_method_for_selector);
    M("conformsToProtocol:",ns_conforms);
#undef M
#define C(s,f) rt_method("NSObject",s,reinterpret_cast<void*>(f),true)
    C("alloc",ns_alloc);C("allocWithZone:",ns_alloc);C("new",ns_new);C("initialize",ns_initialize);C("class",ns_class);C("superclass",ns_super);C("respondsToSelector:",ns_respond);
    C("instanceMethodForSelector:",ns_instance_method_for_selector);C("instancesRespondToSelector:",ns_instances_respond);
    C("conformsToProtocol:",ns_conforms);
#undef C
    rt_method("NSAutoreleasePool","drain",reinterpret_cast<void*>(pool_drain));
    rt_method("NSBlock","copy",reinterpret_cast<void*>(ns_block_copy));
    rt_method("NSBlock","copyWithZone:",reinterpret_cast<void*>(ns_block_copy));
    rt_method("NSBlock","retain",reinterpret_cast<void*>(ns_block_retain));
    rt_method("NSBlock","release",reinterpret_cast<void*>(ns_block_release));
    rt_method("UIApplication","sharedApplication",reinterpret_cast<void*>(application_shared),true);
    rt_method("UIApplication","delegate",reinterpret_cast<void*>(application_delegate));
    rt_method("UIApplication","statusBarFrame",reinterpret_cast<void*>(application_status_bar));
    rt_method("UIApplication","isStatusBarHidden",reinterpret_cast<void*>(application_status_hidden));
    rt_method("UIApplication","statusBarOrientation",reinterpret_cast<void*>(application_orientation));
    rt_method("UIApplication","setDelegate:",reinterpret_cast<void*>(application_set_delegate));
    // Foundation's concrete immutable CF strings inherit NSString behavior.
    static_cast<Class*>(rt_class("NSConstantString"))->super=static_cast<Class*>(rt_class("NSString"));
    static_cast<Class*>(rt_class("__NSCFConstantString"))->super=static_cast<Class*>(rt_class("NSString"));
}

struct BlockDesc {uintptr_t reserved,size;void(*copy)(void*,void*);void(*dispose)(void*);};
struct Block {void*isa;uint32_t flags,reserved;void(*invoke)(void*);BlockDesc*desc;};
static std::unordered_map<void*,unsigned> block_refs;
static std::recursive_mutex block_mutex;
static void* block_copy(void* p){if(!p)return p;std::lock_guard<std::recursive_mutex>guard(block_mutex);auto b=static_cast<Block*>(p);if(b->flags&(1u<<28))return p;auto i=block_refs.find(p);if(i!=block_refs.end()){++i->second;return p;}if(!b->desc||b->desc->size<sizeof(Block)||b->desc->size>1024*1024)rt_fail("invalid block descriptor");void*q=malloc(b->desc->size);if(!q)rt_fail("Block allocation failed");memcpy(q,p,b->desc->size);block_refs[q]=1;if((b->flags&(1u<<25))&&b->desc->copy)b->desc->copy(q,p);return q;}
static void block_release(void*p){std::lock_guard<std::recursive_mutex>guard(block_mutex);auto i=block_refs.find(p);if(i==block_refs.end())return;if(--i->second)return;auto b=static_cast<Block*>(p);if((b->flags&(1u<<25))&&b->desc->dispose)b->desc->dispose(p);block_refs.erase(i);free(p);}
void* rt_block_copy(void*p){return block_copy(p);}
void rt_block_release(void*p){block_release(p);}
static Obj ns_block_copy(Obj o,Sel,void*){return block_copy(o);}
static Obj ns_block_retain(Obj o,Sel){std::lock_guard<std::recursive_mutex>guard(block_mutex);auto it=block_refs.find(o);if(it!=block_refs.end())++it->second;return o;}
static void ns_block_release(Obj o,Sel){block_release(o);}
// Apple Blocks ABI: compiler-generated __block cells use a forwarding pointer.
// The first heap promotion owns two references: the stack scope and its Block.
// See apple-oss-distributions/libdispatch/src/BlocksRuntime/{runtime.c,Block_private.h}.
struct BlockByref {void*isa;BlockByref*forwarding;uint32_t flags,size;};
struct BlockByrefHelpers {void(*keep)(BlockByref*,BlockByref*);void(*destroy)(BlockByref*);};
static constexpr uint32_t byref_count_mask=0xfffe,byref_deallocating=1,
    byref_needs_free=1u<<24,byref_has_helpers=1u<<25,byref_gc=1u<<27,
    byref_layout_mask=0xfu<<28,byref_layout_extended=1u<<28;
static void* byref_weak_class[32]; // ABI marker; GC scanning is not used on iOS.
static BlockByref* block_byref_forward(void*p){
    auto b=static_cast<BlockByref*>(p);
    if(!b||!b->forwarding)rt_fail("invalid Block byref forwarding pointer");
    b=b->forwarding;
    if(b->forwarding!=b||b->size<sizeof(BlockByref)||b->size>1024*1024)
        rt_fail("invalid Block byref header");
    if(b->flags&byref_gc)rt_fail("garbage-collected Block byref is unsupported");
    size_t header=sizeof(BlockByref);
    if(b->flags&byref_has_helpers)header+=sizeof(BlockByrefHelpers);
    if((b->flags&byref_layout_mask)==byref_layout_extended)header+=sizeof(void*);
    if(b->size<header)rt_fail("truncated Block byref helper/layout header");
    if(b->flags&byref_has_helpers){auto h=reinterpret_cast<BlockByrefHelpers*>(b+1);
        if(!h->keep||!h->destroy)rt_fail("invalid Block byref copy/dispose helper");}
    return b;
}
static void block_byref_assign(void*dst,void*src,bool weak){
    std::lock_guard<std::recursive_mutex>guard(block_mutex);
    auto b=block_byref_forward(src);
    if(b->flags&byref_needs_free){
        auto count=b->flags&byref_count_mask;
        if(!count||(b->flags&byref_deallocating))rt_fail("copy of deallocating Block byref");
        if(count!=byref_count_mask)b->flags+=2;
    }else{
        if(b->flags&(byref_count_mask|byref_deallocating))rt_fail("invalid stack Block byref refcount");
        auto heap=static_cast<BlockByref*>(malloc(b->size));
        if(!heap)rt_fail("Block byref allocation failed");
        memcpy(heap,b,b->size);
        heap->flags|=byref_needs_free|4;
        heap->forwarding=heap;
        if(weak)heap->isa=byref_weak_class;
        b->forwarding=heap;
        if(b->flags&byref_has_helpers)
            reinterpret_cast<BlockByrefHelpers*>(b+1)->keep(heap,b);
        b=heap;
    }
    *static_cast<void**>(dst)=b;
}
static void block_byref_dispose(void*src){
    if(!src)return;
    std::lock_guard<std::recursive_mutex>guard(block_mutex);
    auto b=block_byref_forward(src);
    // A synchronous Block often never escapes: its stack scope still emits this
    // call, but the original compiler owns destruction of unpromoted contents.
    if(!(b->flags&byref_needs_free))return;
    auto count=b->flags&byref_count_mask;
    if(!count||(b->flags&byref_deallocating))rt_fail("Block byref reference underflow");
    if(count==byref_count_mask)return; // saturated counts latch, matching Apple.
    b->flags-=2;
    if(count!=2)return;
    b->flags|=byref_deallocating;
    if(b->flags&byref_has_helpers)
        reinterpret_cast<BlockByrefHelpers*>(b+1)->destroy(b);
    free(b);
}
static void block_assign(void*dst,void*src,int flags){
    if(!dst)rt_fail("Block assign with null destination");
    switch(flags){
    case 3:*static_cast<void**>(dst)=ns_retain(src,nullptr);return;
    case 7:*static_cast<void**>(dst)=block_copy(src);return;
    case 8:case 24:block_byref_assign(dst,src,flags==24);return;
    // MRC byref helper fields are deliberately unretained. ARC emits its own
    // object/weak helper calls; these legacy flags do not request ARC ownership.
    case 131:case 135:case 147:case 151:*static_cast<void**>(dst)=src;return;
    default:rt_fail("unsupported Block object assign flags");
    }
}
static void block_dispose(void*src,int flags){
    switch(flags){
    case 3:ns_release(src,nullptr);return;
    case 7:block_release(src);return;
    case 8:case 24:block_byref_dispose(src);return;
    case 131:case 135:case 147:case 151:return;
    default:rt_fail("unsupported Block object dispose flags");
    }
}
static void once(intptr_t*token,void*p){if(*token==-1)return;*token=-1;static_cast<Block*>(p)->invoke(p);}
struct DispatchQueue {
    std::string label;
    bool main=false;
    unsigned references=1;
    std::mutex state;
    std::condition_variable ready;
    uint64_t issued=0,serving=0;
    std::thread::id executing;
};
static auto main_dispatch_queue=[](){auto q=std::make_shared<DispatchQueue>();q->label="com.apple.main-thread";q->main=true;return q;}();
static const std::thread::id main_thread=std::this_thread::get_id();
static std::mutex dispatch_registry_mutex;
static std::unordered_map<void*,std::shared_ptr<DispatchQueue>> dispatch_queues;
struct MainTask {void*block=nullptr;Obj target=nullptr;Sel selector=nullptr;Obj argument=nullptr;};
static std::vector<MainTask> main_queue;
static std::mutex main_queue_mutex;
static std::shared_ptr<DispatchQueue> dispatch_find(void*pointer){
    if(pointer==main_dispatch_queue.get())return main_dispatch_queue;
    std::lock_guard<std::mutex>guard(dispatch_registry_mutex);
    auto i=dispatch_queues.find(pointer);if(i==dispatch_queues.end())rt_fail("unknown dispatch queue");return i->second;
}
static void* dispatch_create(const char*label,void*attributes){
    if(attributes)rt_fail("only serial dispatch_queue_create with NULL attributes is supported");
    auto q=std::make_shared<DispatchQueue>();q->label=label?label:"";
    std::lock_guard<std::mutex>guard(dispatch_registry_mutex);dispatch_queues[q.get()]=q;
    std::fprintf(stderr,"DISPATCH_SERIAL_CREATE %s\n",q->label.c_str());return q.get();
}
static void dispatch_release_queue(void*pointer){
    if(pointer==main_dispatch_queue.get())return;
    std::lock_guard<std::mutex>guard(dispatch_registry_mutex);auto i=dispatch_queues.find(pointer);
    if(i==dispatch_queues.end())rt_fail("dispatch_release on unknown object");
    if(!--i->second->references)dispatch_queues.erase(i);
}
static void dispatch_retain_queue(void*pointer){
    if(pointer==main_dispatch_queue.get())return;
    std::lock_guard<std::mutex>guard(dispatch_registry_mutex);auto i=dispatch_queues.find(pointer);
    if(i==dispatch_queues.end())rt_fail("dispatch_retain on unknown object");++i->second->references;
}
static void dispatch_sync_queue(void*pointer,void*block){
    auto q=dispatch_find(pointer);
    if(q->main)rt_fail("dispatch_sync to main queue is not implemented (current-main would deadlock)");
    if(!block||!static_cast<Block*>(block)->invoke)rt_fail("dispatch_sync with invalid Block");
    std::unique_lock<std::mutex>lock(q->state);
    if(q->executing==std::this_thread::get_id())rt_fail("recursive dispatch_sync to the same serial queue would deadlock");
    const uint64_t ticket=q->issued++;
    q->ready.wait(lock,[&](){return ticket==q->serving;});
    q->executing=std::this_thread::get_id();lock.unlock();
    struct Completion {
        std::shared_ptr<DispatchQueue> q;
        ~Completion(){std::lock_guard<std::mutex>guard(q->state);q->executing={};++q->serving;q->ready.notify_all();}
    } completion{q};
    static_cast<Block*>(block)->invoke(block);
}
static void async_main(void*pointer,void*p){
    auto q=dispatch_find(pointer);
    if(!q->main)rt_fail("background dispatch_async is not implemented; refusing to run it on the main queue");
    if(!p||!static_cast<Block*>(p)->invoke)rt_fail("dispatch_async with invalid Block");
    void*copy=block_copy(p);std::lock_guard<std::mutex>guard(main_queue_mutex);main_queue.push_back({copy});
}
static void ns_perform_on_thread(Obj object,Sel,Sel selector,Obj thread,Obj argument,bool wait){
    if(!thread||thread!=send<Obj>(rt_class("NSThread"),"mainThread"))rt_fail("performSelector target thread is unsupported");
    if(wait){
        if(std::this_thread::get_id()!=main_thread)rt_fail("cross-thread synchronous performSelector is unsupported");
        send<void>(object,selector,argument);return;
    }
    ns_retain(object,nullptr);ns_retain(argument,nullptr);
    std::lock_guard<std::mutex>guard(main_queue_mutex);main_queue.push_back({nullptr,object,selector,argument});
}
static void ns_perform_on_main(Obj object,Sel command,Sel selector,Obj argument,bool wait){ns_perform_on_thread(object,command,selector,send<Obj>(rt_class("NSThread"),"mainThread"),argument,wait);}
void rt_drain_main_queue(){
    if(std::this_thread::get_id()!=main_thread)rt_fail("main dispatch queue drained off the main thread");
    std::vector<MainTask>q;{std::lock_guard<std::mutex>guard(main_queue_mutex);q.swap(main_queue);}
    for(auto task:q){if(task.block){static_cast<Block*>(task.block)->invoke(task.block);block_release(task.block);}else{send<void>(task.target,task.selector,task.argument);ns_release(task.argument,nullptr);ns_release(task.target,nullptr);}}
}
static int mutex_lock(void*p){auto&i=mutexes[p];if(!i)i.reset(new std::recursive_mutex);i->lock();return 0;}
static int mutex_unlock(void*p){auto i=mutexes.find(p);if(i==mutexes.end())rt_fail("unlock unknown mutex");i->second->unlock();return 0;}
static Obj objc_getclass(const char*n){auto i=classes.find(n);return i==classes.end()?nullptr:i->second;}
static Obj objc_meta(const char*n){return static_cast<Class*>(rt_class(n))->isa;}
static const char* class_name(Obj c){return c?ro(static_cast<Class*>(c))->name:"nil";}
static Obj class_super(Obj c){return c?static_cast<Class*>(c)->super:nullptr;}
static Obj object_class(Obj o){return o?*static_cast<Obj*>(o):nullptr;}
static Sel sel_uid(const char*s){return s;}
static Obj getproperty(Obj o,Sel,ptrdiff_t offset,bool){return *reinterpret_cast<Obj*>(static_cast<char*>(o)+offset);}
static void setproperty(Obj o,Sel,Obj value,ptrdiff_t offset){Obj*dst=reinterpret_cast<Obj*>(static_cast<char*>(o)+offset);ns_retain(value,nullptr);ns_release(*dst,nullptr);*dst=value;}
static void setproperty_copy(Obj o,Sel s,Obj value,ptrdiff_t offset){setproperty(o,s,send<Obj>(value,"copy"),offset);}
static void enumeration_mutation(Obj){rt_fail("collection mutated during fast enumeration");}
static Obj nsclassfromstring(Obj s){return objc_getclass(rt_utf8(s));}
static Obj stringfromselector(Sel s){return rt_string(s);}
static void image_callback(void(*fn)(void*,intptr_t)){if(!rt_image)rt_fail("image callback before image loaded");fn(rt_image->base,rt_image->slide);}

static std::string format_stack(const char*fmt,const uint64_t*args){
    std::string result;unsigned ai=0;
    for(size_t i=0;fmt[i];){if(fmt[i]!='%'){result+=fmt[i++];continue;}size_t begin=i++;if(fmt[i]=='%'){result+='%';++i;continue;}
        while(fmt[i]&&strchr("0123456789.-+ #hlzjt",fmt[i]))++i;
        char kind=fmt[i];if(!kind)break;std::string spec(fmt+begin,i-begin+1);++i;
        if(ai>=64)rt_fail("format argument limit");uint64_t v=args[ai++];char buf[4096];
        if(kind=='@'){
            Obj object=reinterpret_cast<Obj>(v);
            if(!object)result+="(null)";
            else {
                const char* name=rt_class_name(object);
                bool string=ns_kind(object,nullptr,rt_class("NSString"))||!strcmp(name,"NSConstantString")||!strcmp(name,"__NSCFConstantString");
                result+=rt_utf8(string?object:send<Obj>(object,"description"));
            }
        }
        else if(kind=='s')result+=v?reinterpret_cast<const char*>(v):"(null)";
        else if(strchr("di",kind)){snprintf(buf,sizeof(buf),"%lld",(long long)v);result+=buf;}
        else if(strchr("uoxX",kind)){snprintf(buf,sizeof(buf),kind=='u'?"%llu":"%llx",(unsigned long long)v);result+=buf;}
        else if(strchr("fFeEgGaA",kind)){double d;memcpy(&d,&v,8);snprintf(buf,sizeof(buf),spec.c_str(),d);result+=buf;}
        else if(kind=='p'){snprintf(buf,sizeof(buf),"%p",reinterpret_cast<void*>(v));result+=buf;}
        else if(kind=='c')result+=char(v);
        else rt_fail("unsupported printf specifier");
    }return result;
}
extern "C" int rt_printf_stack(const char*fmt,const uint64_t*args){auto s=format_stack(fmt,args);return printf("%s",s.c_str());}
extern "C" void rt_nslog_stack(Obj fmt,const uint64_t*args){auto s=format_stack(rt_utf8(fmt),args);fprintf(stderr,"NSLOG %s\n",s.c_str());}
extern "C" [[noreturn]] void rt_unimplemented(const char*symbol){fprintf(stderr,"MISSING_SYMBOL %s caller=%p\n",symbol,__builtin_return_address(0));rt_fail("import invoked before implementation");}
extern "C" [[noreturn]] void rt_guest_abort(){fprintf(stderr,"GUEST_ABORT caller=%p\n",__builtin_return_address(0));abort();}
static void* trap_for(const char*name){auto i=traps.find(name);if(i!=traps.end())return i->second;auto p=static_cast<uint8_t*>(mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));if(p==MAP_FAILED)rt_fail("trap mmap failed");uint32_t code[]={0x58000080,0x580000b1,0xd61f0220,0xd503201f};memcpy(p,code,16);auto label=strdup(name);memcpy(p+16,&label,8);auto f=&rt_unimplemented;memcpy(p+24,&f,8);__builtin___clear_cache(reinterpret_cast<char*>(p),reinterpret_cast<char*>(p+32));if(mprotect(p,4096,PROT_READ|PROT_EXEC))rt_fail("trap mprotect failed");traps[name]=p;return p;}
static int application_main(int argc,char**argv,Obj principal,Obj delegate_name){
    fprintf(stderr,"UIApplicationMain delegate=%s\n",rt_utf8(delegate_name));
    Obj app=application_shared(nullptr,nullptr);Obj delegate=send<Obj>(rt_class(rt_utf8(delegate_name)),"new");
    rt_host(app).target=delegate;
    bool launched=send<bool>(delegate,"application:didFinishLaunchingWithOptions:",app,Obj(nullptr));
    if(!launched)rt_fail("application delegate rejected launch");
    if(rt_responds(delegate,"applicationDidBecomeActive:"))send<void>(delegate,"applicationDidBecomeActive:",app);
    rt_graphics_run_loop();return 0;
}
void* rt_resolve(const char*name,bool weak,void*) {
    if(!strncmp(name,"_OBJC_CLASS_$_",14))return rt_class(name+14);
    if(!strncmp(name,"_OBJC_METACLASS_$_",18))return static_cast<Class*>(rt_class(name+18))->isa;
    if(!strcmp(name,"___CFConstantStringClassReference"))return rt_class("NSConstantString");
    if(!strcmp(name,"__NSConcreteGlobalBlock")||!strcmp(name,"__NSConcreteStackBlock"))return rt_class("NSBlock");
    static uintptr_t empty_cache[2]={};
    if(!strcmp(name,"__objc_empty_cache")||!strcmp(name,"__objc_empty_vtable"))return empty_cache;
#define F(n,f) if(!strcmp(name,n))return reinterpret_cast<void*>(f)
    F("_objc_msgSend",objc_msgSend);F("_objc_msgSendSuper2",objc_msgSendSuper2);
    F("_objc_getClass",objc_getclass);F("_objc_lookUpClass",objc_getclass);F("_objc_getRequiredClass",objc_getclass);F("_objc_getMetaClass",objc_meta);
    F("_class_getName",class_name);F("_class_getSuperclass",class_super);F("_object_getClass",object_class);F("_sel_getUid",sel_uid);
    F("_protocol_getMethodDescription",protocol_description);F("_protocol_getName",protocol_name);F("_objc_getProtocol",protocol_get);
    F("_class_replaceMethod",class_replace);F("_class_addMethod",class_add);F("_class_getInstanceMethod",class_instance_method);F("_method_setImplementation",method_replace);F("_class_isMetaClass",class_meta);
    F("_objc_readClassPair",read_class_pair);
    F("_objc_getProperty",getproperty);F("_objc_setProperty_atomic",setproperty);F("_objc_setProperty_nonatomic",setproperty);F("_objc_setProperty_nonatomic_copy",setproperty_copy);
    F("_objc_sync_enter",mutex_lock);F("_objc_sync_exit",mutex_unlock);F("_pthread_mutex_lock",mutex_lock);F("_pthread_mutex_unlock",mutex_unlock);
    F("_objc_enumerationMutation",enumeration_mutation);F("__Block_object_assign",block_assign);F("__Block_object_dispose",block_dispose);
    F("_dispatch_once",once);F("_dispatch_async",async_main);
    F("_dispatch_queue_create",dispatch_create);F("_dispatch_sync",dispatch_sync_queue);F("_dispatch_release",dispatch_release_queue);F("_dispatch_retain",dispatch_retain_queue);
    F("_NSClassFromString",nsclassfromstring);F("_NSStringFromSelector",stringfromselector);
    F("_printf",rt_printf_bridge);F("_NSLog",rt_nslog_bridge);F("_UIApplicationMain",application_main);
    F("_abort",rt_guest_abort);
    F("__dyld_register_func_for_add_image",image_callback);
#undef F
    if(void*p=rt_graphics_symbol(name))return p;
    if(void*p=rt_foundation_symbol(name))return p;
    if(void*p=rt_services_symbol(name))return p;
    if(void*p=rt_audio_symbol(name))return p;
    if(void*p=rt_darwin_symbol(name))return p;
    if(!strcmp(name,"___stderrp"))return &stderr;
    static uintptr_t stack_guard=0x781abcd032e9; if(!strcmp(name,"___stack_chk_guard"))return &stack_guard;
    static double cfversion=847.20;if(!strcmp(name,"_kCFCoreFoundationVersionNumber"))return &cfversion;
    if(!strcmp(name,"__dispatch_main_q"))return main_dispatch_queue.get();
    static const std::unordered_set<std::string> string_constants={"_NSDefaultRunLoopMode","_NSInternalInconsistencyException","_UIApplicationDidBecomeActiveNotification","_UIApplicationDidEnterBackgroundNotification","_GCControllerDidConnectNotification","_GCControllerDidDisconnectNotification","_GKPlayerAuthenticationDidChangeNotificationName","_AVAudioSessionCategoryAmbient","_AVAudioSessionCategoryPlayAndRecord","_AVAudioSessionCategoryPlayback","_AVAudioSessionCategorySoloAmbient","_kEAGLColorFormatRGB565","_kEAGLDrawablePropertyColorFormat","_kEAGLDrawablePropertyRetainedBacking"};
    if(string_constants.count(name)){
        auto&i=constants[name];if(!i)i=new Obj(rt_string(name+1));return i;
    }
    // Darwin structures are bridged above. Remaining selected C math/memory/file APIs use opaque data.
    const char*host_name=name[0]=='_'?name+1:name;
    static const std::unordered_set<std::string> direct={"abort","acosf","arc4random","asinf","atan2f","bsearch","bzero","calloc","cos","cosf","exit","exp2f","expf","fclose","ferror","fmodf","fopen","fread","free","fseek","ftell","fwrite","gettimeofday","gzclose","gzopen","gzread","inflate","inflateEnd","inflateInit2_","logf","malloc","memchr","memcmp","memcpy","memmove","modf","powf","puts","qsort","rand","random","realloc","sin","sinf","srand","strcmp","strlen","strncmp","tanf","uncompress","__stack_chk_fail","_ZdlPv","_Znwm","_ZSt9terminatev","__cxa_begin_catch","__cxa_pure_virtual","__gxx_personality_v0","_Unwind_Resume","_ZTVN10__cxxabiv117__class_type_infoE","_ZTVN10__cxxabiv120__si_class_type_infoE"};
    if(direct.count(host_name)){void*p=dlsym(RTLD_DEFAULT,host_name);if(p)return p;}
    fprintf(stderr,"IMPORT_TRAP %s%s\n",name,weak?" weak":"");return trap_for(name);
}
void rt_call_load_methods(){
    auto s=rt_image->find_section("__DATA","__objc_nlclslist");if(!s)return;
    auto arr=static_cast<Class**>(s->address);
    for(size_t i=0;i<s->size/8;++i)if(void*f=own_method(arr[i]->isa,"load")){
        fprintf(stderr,"OBJC_LOAD %s\n",ro(arr[i])->name);reinterpret_cast<void(*)(Obj,Sel)>(f)(arr[i],"load");}
}
