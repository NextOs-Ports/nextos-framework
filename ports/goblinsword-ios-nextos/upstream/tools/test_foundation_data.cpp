// Host-only harness: supplies tiny Objective-C host objects, never guest code.
// The Python driver compares decoded IPA plists against plistlib and the SAX
// callback stream against ElementTree. No Apple libraries are linked.
#include "../prototype/foundation.cpp"
#include <iostream>
#include <stdexcept>

namespace harness {
struct Object { std::string name; bool cls=false; };
std::unordered_map<Obj,HostData> data;
std::unordered_map<std::string,Obj> classes;
std::unordered_map<std::string,void*> methods;
std::string key(const char* cls,Sel sel){return std::string(cls)+":"+sel;}
Obj retain(Obj obj,Sel){++data[obj].references;return obj;}
void release(Obj obj,Sel){if(--data[obj].references==0){rt_foundation_dealloc(obj);data.erase(obj);delete static_cast<Object*>(obj);}}
bool equal(Obj a,Sel,Obj b){return a==b;}
bool responds(Obj obj,Sel,Sel sel){return methods.count(key(rt_class_name(obj),sel));}
std::string quoted(const std::string& value){std::string result="\"";for(unsigned char c:value){if(c=='"'||c=='\\'){result+='\\';result+=c;}else if(c<32){char temp[7];std::snprintf(temp,sizeof temp,"\\u%04x",c);result+=temp;}else result+=c;}return result+'"';}
void dump(std::ostream& out,Obj value){
    auto cls=std::string(rt_class_name(value));
    if(cls=="NSNumber"){out.precision(17);out<<rt_host(value).number;}
    else if(cls.find("String")!=std::string::npos)out<<quoted(rt_utf8(value));
    else if(cls=="NSArray"||cls=="NSMutableArray"){out<<'[';bool first=true;for(Obj v:rt_host(value).array){if(!first)out<<',';first=false;dump(out,v);}out<<']';}
    else if(cls=="NSDictionary"||cls=="NSMutableDictionary"){out<<'{';bool first=true;for(auto& kv:rt_host(value).dictionary){if(!first)out<<',';first=false;out<<quoted(kv.first)<<':';dump(out,kv.second);}out<<'}';}
    else if(cls=="NSData"){out<<"{\"__data_hex__\":\"";for(unsigned char c:rt_host(value).text){char temp[3];std::snprintf(temp,sizeof temp,"%02x",c);out<<temp;}out<<"\"}";}
    else if(cls=="NSNull")out<<"null";
    else throw std::runtime_error("harness unexpected class "+cls);
}
std::vector<std::string> events;
void xml_start(Obj,Sel,Obj,Obj name,Obj,Obj,Obj attrs){std::ostringstream output;output<<"[\"start\","<<quoted(rt_utf8(name))<<',';dump(output,attrs);output<<']';events.push_back(output.str());}
void xml_end(Obj,Sel,Obj,Obj name,Obj,Obj){events.push_back("[\"end\","+quoted(rt_utf8(name))+"]");}
void xml_chars(Obj,Sel,Obj,Obj chars){events.push_back("[\"text\","+quoted(rt_utf8(chars))+"]");}
}

MachImage* rt_image=nullptr;
std::string rt_data_root;
void* rt_class(const char* name){auto& c=harness::classes[name];if(!c)c=new harness::Object{name,true};return c;}
const char* rt_class_name(Obj obj){return obj?static_cast<harness::Object*>(obj)->name.c_str():"nil";}
Obj rt_new(const char* name){auto obj=new harness::Object{name};harness::data[obj]=HostData{};return obj;}
Obj rt_string(const char* text){Obj obj=rt_new("NSString");rt_host(obj).text=text?text:"";return obj;}
const char* rt_utf8(Obj obj){return obj?rt_host(obj).text.c_str():"";}
HostData& rt_host(Obj obj){return harness::data.at(obj);}
bool rt_responds(Obj obj,Sel sel){return obj&&harness::responds(obj,nullptr,sel);}
const char* rt_method_types(Obj,Sel){return nullptr;}
void rt_method(const char* cls,const char* sel,void* imp,bool){harness::methods[harness::key(cls,sel)]=imp;}
extern "C" void* rt_lookup(Obj obj,Sel sel){
    if(std::strcmp(sel,"retain")==0)return reinterpret_cast<void*>(harness::retain);
    if(std::strcmp(sel,"release")==0)return reinterpret_cast<void*>(harness::release);
    if(std::strcmp(sel,"respondsToSelector:")==0)return reinterpret_cast<void*>(harness::responds);
    auto it=harness::methods.find(harness::key(rt_class_name(obj),sel));if(it!=harness::methods.end())return it->second;
    if(std::strcmp(sel,"isEqual:")==0)return reinterpret_cast<void*>(harness::equal);
    throw std::runtime_error(std::string("harness missing selector ")+rt_class_name(obj)+" "+sel);
}
[[noreturn]]void rt_fail(const char* reason){throw std::runtime_error(reason);}
extern "C" void foundation_string_format(){}
extern "C" void foundation_array_objects(){}
extern "C" void foundation_dictionary_objects_keys(){}

int main(int argc,char** argv){
    try {
        if(argc<2||argc>3)throw std::runtime_error("usage: test_foundation_data --plist|--xml|--strings|--defaults [scratch-directory]");
        rt_install_foundation();
        std::string input{std::istreambuf_iterator<char>(std::cin),std::istreambuf_iterator<char>()};
        if(std::strcmp(argv[1],"--plist")==0){Obj root=parse_plist(input);harness::dump(std::cout,root);drop(root);std::cout<<'\n';}
        else if(std::strcmp(argv[1],"--xml")==0){
            rt_method("TestDelegate","parser:didStartElement:namespaceURI:qualifiedName:attributes:",reinterpret_cast<void*>(harness::xml_start));
            rt_method("TestDelegate","parser:didEndElement:namespaceURI:qualifiedName:",reinterpret_cast<void*>(harness::xml_end));
            rt_method("TestDelegate","parser:foundCharacters:",reinterpret_cast<void*>(harness::xml_chars));
            Obj parser=rt_new("NSXMLParser");rt_host(parser).text=input;rt_host(parser).target=rt_new("TestDelegate");xml_parse(parser,nullptr);
            std::cout<<'[';bool first=true;for(auto& event:harness::events){if(!first)std::cout<<',';first=false;std::cout<<event;}std::cout<<"]\n";
        }else if(std::strcmp(argv[1],"--strings")==0){
            Obj text=ns("A😀áZ");if(string_length(text,nullptr)!=5||str(string_substring(text,nullptr,{1,2}))!="😀"||string_character(text,nullptr,3)!=0xe1)throw std::runtime_error("UTF16 index mismatch");
            Obj fmt=ns("%@=%02d/%.2f");Obj key=ns("value");double value=1.25;uint64_t args[3]={reinterpret_cast<uint64_t>(key),7,0};std::memcpy(&args[2],&value,8);
            if(str(foundation_string_format_stack(nullptr,"stringWithFormat:",fmt,args))!="value=07/1.25")throw std::runtime_error("Darwin argument stack formatting mismatch");
            Obj child=ns("child"),parent=array();array_add(parent,nullptr,child);drop(child);if(str(array_at(parent,nullptr,0))!="child")throw std::runtime_error("collection ownership mismatch");drop(parent);
            std::cout<<"{\"utf16\":true,\"darwin_stack_format\":true,\"collection_retains\":true}\n";
        }else if(std::strcmp(argv[1],"--fntstrings")==0){
            Obj text=rt_new("NSMutableString");if(send<Obj>(text,"initWithCapacity:",uint64_t(512))!=text||string_length(text,nullptr)!=0)throw std::runtime_error("NSMutableString initWithCapacity mismatch");
            Obj factory=send<Obj>(rt_class("NSMutableString"),"stringWithCapacity:",uint64_t(32));if(string_length(factory,nullptr)!=0||rt_host(factory).text.capacity()<32)throw std::runtime_error("NSMutableString capacity factory mismatch");
            Obj empty=send<Obj>(rt_class("NSString"),"string");if(string_length(empty,nullptr)!=0)throw std::runtime_error("NSString string mismatch");
            uint64_t code[]={0xe1};Obj glyph=foundation_string_format_stack(nullptr,"stringWithFormat:",ns("%C"),code);if(str(glyph)!="á")throw std::runtime_error("FNT %C mismatch");
            Obj joined=foundation_string_format_stack(ns("font:"),"stringByAppendingFormat:",ns("%C"),code);if(str(joined)!="font:á")throw std::runtime_error("stringByAppendingFormat mismatch");
            Obj set=character_set(nullptr,nullptr,ns("AáZ"));if(!send<bool>(set,"characterIsMember:",uint16_t(0xe1))||send<bool>(set,"characterIsMember:",uint16_t('B')))throw std::runtime_error("character membership mismatch");
            Obj newline=send<Obj>(rt_class("NSCharacterSet"),"newlineCharacterSet");if(!send<bool>(newline,"characterIsMember:",uint16_t('\n'))||!send<bool>(newline,"characterIsMember:",uint16_t(0x2028)))throw std::runtime_error("newline set mismatch");
            Obj spaces=send<Obj>(rt_class("NSCharacterSet"),"whitespaceCharacterSet");if(send<bool>(spaces,"characterIsMember:",uint16_t('\n')))throw std::runtime_error("whitespace incorrectly includes newline");
            Obj split=string_components_set(ns("á\u2028©"),nullptr,newline);if(rt_host(split).array.size()!=2||str(rt_host(split).array[0])!="á"||str(rt_host(split).array[1])!="©")throw std::runtime_error("Unicode newline split mismatch");
            if(str(string_trim(ns("\u00a0©\u00a0"),nullptr,spaces))!="©")throw std::runtime_error("Unicode trim mismatch");
            std::cout<<"{\"status\":\"PASS\",\"mutable_capacity_init_and_factory\":true,\"empty_string_factory\":true,\"fnt_percent_C\":true,\"appending_format\":true,\"unichar_membership\":true,\"newline_whitespace_separation\":true,\"unicode_split_trim\":true,\"executes_guest_code\":false}\n";
        }else if(std::strcmp(argv[1],"--enumerator")==0){
            std::vector<Obj> objects;for(int i=0;i<7;++i)objects.push_back(number(i));
            Obj source=array(objects),iterator=send<Obj>(source,"objectEnumerator");drop(source);
            EnumerationState state{},fresh{};Obj buffer[3]{};constexpr Sel fast="countByEnumeratingWithState:objects:count:";
            auto batch=[&](Obj e,EnumerationState& s,uint64_t n,std::initializer_list<Obj> wanted){uint64_t got=send<uint64_t>(e,fast,&s,buffer,n);if(got!=wanted.size()||!std::equal(wanted.begin(),wanted.end(),buffer))throw std::runtime_error("NSEnumerator batch/cursor mismatch");if(got&&(s.items!=buffer||!s.mutations))throw std::runtime_error("NSEnumerator state pointers mismatch");};
            if(send<uint64_t>(iterator,fast,&state,static_cast<Obj*>(nullptr),uint64_t(0))!=0||send<Obj>(iterator,"nextObject")!=objects[0])throw std::runtime_error("NSEnumerator zero-capacity/nextObject mismatch");
            batch(iterator,state,2,{objects[1],objects[2]});auto* mutation=state.mutations;auto version=*mutation;
            if(send<Obj>(iterator,"nextObject")!=objects[3])throw std::runtime_error("NSEnumerator nextObject after batch mismatch");
            batch(iterator,state,2,{objects[4],objects[5]});batch(iterator,fresh,3,{objects[6]});
            if(state.mutations!=mutation||fresh.mutations!=mutation||*mutation!=version)throw std::runtime_error("NSEnumerator consumption changed mutation identity");
            batch(iterator,state,3,{});fresh={};batch(iterator,fresh,3,{});if(send<Obj>(iterator,"nextObject"))throw std::runtime_error("NSEnumerator exhaustion mismatch");
            for(Obj o:objects)if(rt_host(o).references!=2)throw std::runtime_error("NSEnumerator borrowed return ownership mismatch");
            drop(iterator);for(Obj o:objects)if(rt_host(o).references!=1)throw std::runtime_error("NSEnumerator release ownership mismatch");
            source=array(objects);iterator=send<Obj>(source,"reverseObjectEnumerator");drop(source);state={};
            if(send<Obj>(iterator,"nextObject")!=objects[6])throw std::runtime_error("reverse NSEnumerator nextObject mismatch");
            batch(iterator,state,3,{objects[5],objects[4],objects[3]});batch(iterator,state,3,{objects[2],objects[1],objects[0]});batch(iterator,state,3,{});drop(iterator);
            for(Obj o:objects){if(rt_host(o).references!=1)throw std::runtime_error("reverse NSEnumerator ownership mismatch");drop(o);}
            iterator=rt_new("NSEnumerator");state={};batch(iterator,state,3,{});if(send<Obj>(iterator,"nextObject"))throw std::runtime_error("empty NSEnumerator mismatch");drop(iterator);
            std::cout<<"{\"status\":\"PASS\",\"mixed_nextObject_fast_enumeration\":true,\"variable_batches\":true,\"new_state_keeps_cursor\":true,\"exhaustion\":true,\"reverse_order\":true,\"empty\":true,\"state_pointers_stable\":true,\"borrowed_values_ownership\":true,\"snapshot_release\":true,\"executes_guest_code\":false}\n";
        }else if(std::strcmp(argv[1],"--sort")==0){
            struct SortContext {int direction;unsigned calls=0;bool active=true;};
            SortContext context{1};
            ArrayComparator compare=+[](Obj a,Obj b,void* opaque)->int64_t{
                auto& ctx=*static_cast<SortContext*>(opaque);if(!ctx.active)throw std::runtime_error("sort callback escaped synchronous call");++ctx.calls;
                if(rt_host(a).references!=2||rt_host(b).references!=2)throw std::runtime_error("sort changed callback object ownership");
                double lhs=rt_host(a).number,rhs=rt_host(b).number;return lhs==rhs?0:(lhs<rhs?-(int64_t(1)<<40):(int64_t(1)<<40))*ctx.direction;
            };
            std::vector<Obj> objects={number(4),number(-3),number(2),number(0),number(2)};Obj values=array(objects);
            auto before=mutation_words[values];send<void>(values,"sortUsingFunction:context:",compare,static_cast<void*>(&context));
            std::vector<Obj> expected={objects[1],objects[3],objects[2],objects[4],objects[0]};
            if(rt_host(values).array!=expected||!context.calls||mutation_words[values]==before)throw std::runtime_error("sort order/ties/mutation mismatch");
            context.direction=-1;send<void>(values,"sortUsingFunction:context:",compare,static_cast<void*>(&context));
            expected={objects[0],objects[2],objects[4],objects[3],objects[1]};if(rt_host(values).array!=expected)throw std::runtime_error("sort context direction mismatch");
            unsigned calls=context.calls;context.active=false;for(Obj o:objects)if(rt_host(o).references!=2)throw std::runtime_error("sort final ownership mismatch");drop(values);
            for(Obj o:objects){if(rt_host(o).references!=1)throw std::runtime_error("sort array release ownership mismatch");drop(o);}
            if(context.calls!=calls)throw std::runtime_error("sort retained callback");
            Obj empty=array();context.active=true;send<void>(empty,"sortUsingFunction:context:",compare,static_cast<void*>(&context));if(context.calls!=calls)throw std::runtime_error("empty sort invoked comparator");drop(empty);
            std::cout<<"{\"status\":\"PASS\",\"signed_64bit_comparator\":true,\"negative_values\":true,\"stable_ties\":true,\"opaque_context\":true,\"synchronous_callback\":true,\"ownership_preserved\":true,\"enumeration_mutation_recorded\":true,\"empty_array\":true,\"executes_guest_code\":false}\n";
        }else if(std::strcmp(argv[1],"--icloud")==0){
            Obj cache=send<Obj>(rt_class("NSUbiquitousKeyValueStore"),"defaultStore"),key=ns("stage");
            if(cache!=send<Obj>(rt_class("NSUbiquitousKeyValueStore"),"defaultStore")||cache==singleton("NSUserDefaults"))throw std::runtime_error("iCloud singleton isolation mismatch");
            if(send<Obj>(cache,"objectForKey:",key)||send<bool>(cache,"boolForKey:",key)||send<double>(cache,"doubleForKey:",key)!=0)throw std::runtime_error("iCloud missing value mismatch");
            send<void>(cache,"setDouble:forKey:",2.5,key);Obj boxed=send<Obj>(cache,"objectForKey:",key);
            if(send<double>(cache,"doubleForKey:",key)!=2.5||rt_host(boxed).references!=1)throw std::runtime_error("iCloud numeric value/ownership mismatch");
            send<void>(cache,"setBool:forKey:",true,key);if(harness::data.count(boxed)||!send<bool>(cache,"boolForKey:",key))throw std::runtime_error("iCloud numeric replacement mismatch");
            Obj value=ns("local");send<void>(cache,"setObject:forKey:",value,key);if(rt_host(value).references!=2)throw std::runtime_error("iCloud object retain mismatch");drop(value);
            Obj snapshot=send<Obj>(cache,"dictionaryRepresentation");send<void>(cache,"removeObjectForKey:",key);
            if(send<Obj>(cache,"objectForKey:",key)||str(dict_get(snapshot,nullptr,key))!="local"||rt_host(value).references!=1)throw std::runtime_error("iCloud snapshot/remove ownership mismatch");
            drop(snapshot);if(harness::data.count(value))throw std::runtime_error("iCloud snapshot release mismatch");
            send<void>(cache,"setBool:forKey:",true,key);send<void>(cache,"setObject:forKey:",static_cast<Obj>(nullptr),key);if(send<Obj>(cache,"objectForKey:",key))throw std::runtime_error("iCloud nil setter removal mismatch");
            if(send<Obj>(singleton("NSUserDefaults"),"objectForKey:",key)||send<bool>(cache,"synchronize"))throw std::runtime_error("iCloud offline/defaults isolation mismatch");
            Obj temporary=rt_new("NSUbiquitousKeyValueStore"),child=ns("owned");send<void>(temporary,"setObject:forKey:",child,key);drop(child);drop(temporary);if(harness::data.count(child))throw std::runtime_error("iCloud dealloc ownership mismatch");
            std::cout<<"{\"status\":\"PASS\",\"offline_sync_false\":true,\"separate_defaults\":true,\"missing_nil_zero\":true,\"local_values\":true,\"retained_values\":true,\"snapshot_independent\":true,\"replacement_removal_dealloc_release\":true,\"executes_guest_code\":false}\n";
        }else if(std::strcmp(argv[1],"--numbers")==0){
            Obj n=rt_new("NSNumber");
            auto check=[&](Obj returned,double expected){if(returned!=n||rt_host(n).number!=expected||rt_host(n).references!=1)throw std::runtime_error("NSNumber initializer identity/value/ownership mismatch");};
            check(send<Obj>(n,"initWithInt:",int32_t(-17)),-17);
            check(send<Obj>(n,"initWithInteger:",int64_t(-1099511627776LL)),-1099511627776.0);
            check(send<Obj>(n,"initWithLongLong:",int64_t(1099511627776LL)),1099511627776.0);
            check(send<Obj>(n,"initWithUnsignedInt:",uint32_t(0xffffffffU)),4294967295.0);
            check(send<Obj>(n,"initWithUnsignedInteger:",uint64_t(1099511627776ULL)),1099511627776.0);
            check(send<Obj>(n,"initWithBool:",true),1);
            check(send<Obj>(n,"initWithFloat:",float(-1.25)),-1.25);
            check(send<Obj>(n,"initWithDouble:",double(0.125)),0.125);
            Obj factory=send<Obj>(rt_class("NSNumber"),"numberWithInt:",int32_t(-17));if(rt_host(factory).number!=-17)throw std::runtime_error("NSNumber signed int factory mismatch");
            drop(factory);drop(n);std::cout<<"{\"status\":\"PASS\",\"initializers\":8,\"identity_preserved\":true,\"ownership_preserved\":true,\"negative_int_factory\":true,\"executes_guest_code\":false}\n";
        }else if(std::strcmp(argv[1],"--defaults")==0){
            if(argc!=3)throw std::runtime_error("defaults mode requires scratch directory");rt_data_root=argv[2];
            Obj prefs=defaults_standard(nullptr,nullptr);defaults_set_bool(prefs,nullptr,true,ns("sound"));defaults_set_integer(prefs,nullptr,42,ns("stage"));defaults_set_double(prefs,nullptr,0.75,ns("volume"));dict_set(prefs,nullptr,ns("A&B<á>"),ns("name"));
            if(!defaults_sync(prefs,nullptr))throw std::runtime_error("defaults synchronization failed");
            bool ok;auto bytes=read_file(defaults_path().string(),ok);if(!ok)throw std::runtime_error("defaults persisted file missing");Obj loaded=parse_plist(bytes);
            if(!defaults_bool(loaded,nullptr,ns("sound"))||defaults_integer(loaded,nullptr,ns("stage"))!=42||defaults_double(loaded,nullptr,ns("volume"))!=0.75||str(dict_get(loaded,nullptr,ns("name")))!="A&B<á>")throw std::runtime_error("defaults roundtrip mismatch");
            std::cout<<"{\"defaults_atomic_roundtrip\":true}\n";
        }else throw std::runtime_error("unknown mode");
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
