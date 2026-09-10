// Real Clang-generated Block copy/dispose helpers, without running ARM64 code.
#define main existing_runtime_fixture_main
#include "host_runtime_fixture.cpp"
#undef main
extern "C" {
void* _NSConcreteStackBlock[32];
void* _NSConcreteGlobalBlock[32];
void _Block_object_assign(void*dst,const void*src,int flags){block_assign(dst,const_cast<void*>(src),flags);}
void _Block_object_dispose(const void*src,int flags){block_dispose(const_cast<void*>(src),flags);}
}
using Counter = int(^)(void);
static Counter copy_counter(Counter c){return reinterpret_cast<Counter>(rt_block_copy(reinterpret_cast<void*>(c)));}
static void release_counter(Counter c){rt_block_release(reinterpret_cast<void*>(c));}
struct CounterPair {Counter increment,read;};
static CounterPair make_pair(){
    __block int value=10;
    Counter increment=^{return ++value;},read=^{return value;};
    CounterPair result{copy_counter(increment),copy_counter(read)};
    assert(increment()==11&&read()==11); // Stack access follows the heap cell.
    return result; // Compiler releases its stack ownership of the promoted cell.
}
struct Resource {
    static int live,copies,destroys;
    int value=42;
    Resource(){++live;}
    Resource(const Resource&r):value(r.value){++live;++copies;}
    ~Resource(){--live;++destroys;}
};
int Resource::live=0,Resource::copies=0,Resource::destroys=0;
static Counter make_resource(){
    __block Resource item;
    Counter c=^{return ++item.value;};
    return copy_counter(c);
}
int main(){
    // The device failure: non-escaping sync Block still gets a byref dispose.
    {
        __block int value=3;
        Counter stack=^{return ++value;};
        assert(stack()==4&&value==4);
    }
    assert(block_refs.empty());
    auto pair=make_pair();
    assert(pair.read()==11&&pair.increment()==12&&pair.read()==12);
    auto another=copy_counter(pair.increment);
    assert(another==pair.increment);
    release_counter(pair.increment);
    assert(another()==13&&pair.read()==13);
    release_counter(pair.read);
    assert(another()==14);
    release_counter(another);
    assert(block_refs.empty());
    auto resource=make_resource();
    assert(Resource::live==1&&Resource::copies==1&&Resource::destroys==1);
    assert(resource()==43);
    release_counter(resource);
    assert(Resource::live==0&&Resource::destroys==2&&block_refs.empty());
    // Legacy MRC helper flags preserve unretained fields, including Block fields.
    int marker=7;void*destination=nullptr;
    for(int flags:{131,135,147,151}){
        block_assign(&destination,&marker,flags);assert(destination==&marker);
        block_dispose(&marker,flags);
    }
    // A nonpromoted cell's helper belongs to compiler scope destruction, not us.
    BlockByref untouched{nullptr,nullptr,0,sizeof(BlockByref)};
    untouched.forwarding=&untouched;
    block_dispose(&untouched,8);assert(untouched.forwarding==&untouched&&untouched.flags==0);
    puts("PASS: real Clang byref sync cleanup; shared mutable forwarding across two escaping Blocks; heap Block retain/release; scope release before callbacks; C++ byref copy/destructor exactly once; MRC helper nonownership; no ARM64 execution");
}
