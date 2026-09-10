#include "runtime.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <codecvt>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <locale>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_set>

namespace fs = std::filesystem;
namespace {
struct Range { uint64_t location, length; };
constexpr uint64_t not_found = 0x7fffffffffffffffULL;
struct EnumerationState { unsigned long state; Obj* items; unsigned long* mutations; unsigned long extra[5]; };
struct Observer { Obj target; Sel selector; std::string name; Obj object; };
std::unordered_map<Obj, std::vector<Observer>> observers;
std::unordered_map<Obj, std::unique_ptr<std::recursive_mutex>> locks;
std::unordered_map<std::string, Obj> singletons;
std::unordered_map<Obj, uint64_t> enumeration_position;
std::unordered_map<Obj,unsigned long> mutation_words;
Obj hold(Obj object){return object?send<Obj>(object,"retain"):nullptr;}
void drop(Obj object){if(object)send<void>(object,"release");}
void changed(Obj object){++mutation_words[object];}

Obj singleton(const char* name) {
    auto& result = singletons[name];
    if (!result) result = rt_new(name);
    return result;
}
std::string str(Obj object) { return object ? rt_utf8(object) : ""; }
Obj ns(const std::string& value) { return rt_string(value.c_str()); }
Obj array(const std::vector<Obj>& values = {}) { Obj o = rt_new("NSMutableArray"); for(Obj value:values)rt_host(o).array.push_back(hold(value)); return o; }
Obj dictionary() { return rt_new("NSMutableDictionary"); }
Obj number(double value) { Obj o = rt_new("NSNumber"); rt_host(o).number = value; return o; }
std::u16string utf16(const std::string& value) {
    try { return std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.from_bytes(value); }
    catch (...) { rt_fail("Foundation: invalid UTF-8"); }
}
std::string utf8(const std::u16string& value) {
    try { return std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.to_bytes(value); }
    catch (...) { rt_fail("Foundation: invalid UTF-16"); }
}
std::string read_file(const std::string& filename, bool& ok) {
    std::ifstream input(filename, std::ios::binary);
    ok = bool(input);
    if (!ok) return {};
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
Obj read_data(Obj, Sel, Obj path) {
    bool ok; auto data = read_file(str(path), ok);
    if (!ok) return nullptr;
    Obj result = rt_new("NSData"); rt_host(result).text = std::move(data); return result;
}
std::string xml_unescape(std::string value) {
    std::string result;
    for (size_t i = 0; i < value.size();) {
        if (value[i] != '&') { result += value[i++]; continue; }
        size_t end = value.find(';', i);
        if (end == std::string::npos) rt_fail("Foundation XML: unterminated entity");
        auto entity = value.substr(i + 1, end - i - 1);
        if (entity == "amp") result += '&';
        else if (entity == "lt") result += '<';
        else if (entity == "gt") result += '>';
        else if (entity == "quot") result += '"';
        else if (entity == "apos") result += '\'';
        else if (!entity.empty() && entity[0] == '#') {
            bool hex = entity.size() > 1 && entity[1] == 'x';
            char* endptr = nullptr;
            unsigned long code = std::strtoul(entity.c_str() + (hex ? 2 : 1), &endptr, hex ? 16 : 10);
            if (!endptr || *endptr || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) rt_fail("Foundation XML: invalid character reference");
            std::u16string text;
            if (code < 0x10000) text += char16_t(code);
            else { code -= 0x10000; text += char16_t(0xd800 + (code >> 10)); text += char16_t(0xdc00 + (code & 0x3ff)); }
            result += utf8(text);
        } else rt_fail("Foundation XML: unsupported entity");
        i = end + 1;
    }
    return result;
}

// Binary property-list decoder. Objects remain data; no scripts, code or
// external entities are evaluated. Reject invalid offsets/cycles/types.
class BinaryPlist {
    const std::string& bytes;
    uint64_t objects = 0, top = 0, table = 0;
    unsigned offset_size = 0, reference_size = 0;
    std::vector<Obj> cache;
    std::vector<bool> active;
    uint64_t read(size_t offset, size_t size) const {
        if (!size || size > 8 || offset > bytes.size() || size > bytes.size() - offset) rt_fail("Foundation bplist: invalid integer read");
        uint64_t result = 0;
        for (size_t i = 0; i < size; ++i) result = (result << 8) | static_cast<unsigned char>(bytes[offset + i]);
        return result;
    }
    void bounds(uint64_t offset, uint64_t size) const {
        if (offset > table || size > table - offset) rt_fail("Foundation bplist: object outside object table");
    }
    uint64_t length(unsigned nibble, uint64_t& offset) const {
        if (nibble != 15) return nibble;
        bounds(offset, 1);
        unsigned marker = static_cast<unsigned char>(bytes[offset++]);
        if ((marker >> 4) != 1 || (marker & 15) > 3) rt_fail("Foundation bplist: invalid extended length");
        size_t size = size_t(1) << (marker & 15);
        bounds(offset, size); uint64_t result = read(offset, size); offset += size; return result;
    }
    Obj object(uint64_t index, unsigned depth = 0) {
        if (index >= objects || depth > 128) rt_fail("Foundation bplist: invalid reference/depth");
        if (cache[index]) return cache[index];
        if (active[index]) rt_fail("Foundation bplist: cyclic object graph");
        active[index] = true;
        uint64_t offset = read(table + index * offset_size, offset_size);
        bounds(offset, 1);
        unsigned marker = static_cast<unsigned char>(bytes[offset++]), type = marker >> 4, info = marker & 15;
        Obj result = nullptr;
        if (type == 0) {
            if (info == 8 || info == 9) result = number(info == 9);
            else if (info == 0) result = singleton("NSNull");
            else rt_fail("Foundation bplist: unsupported simple value");
        } else if (type == 1) {
            if (info > 3) rt_fail("Foundation bplist: oversized integer");
            size_t size = size_t(1) << info; bounds(offset, size); uint64_t raw = read(offset, size);
            // Apple short integers are unsigned; negative values use eight bytes.
            result = number(size == 8 ? static_cast<double>(static_cast<int64_t>(raw)) : static_cast<double>(raw));
        } else if (type == 2 || type == 3) {
            size_t size = size_t(1) << info;
            if (size != 4 && size != 8) rt_fail("Foundation bplist: unsupported real width");
            bounds(offset, size); uint64_t raw = read(offset, size); double value;
            if (size == 8) std::memcpy(&value, &raw, 8);
            else { uint32_t word = raw; float small; std::memcpy(&small, &word, 4); value = small; }
            result = number(value);
        } else if (type == 4 || type == 5 || type == 6) {
            uint64_t len = length(info, offset);
            if (type == 6 && len > UINT64_MAX / 2) rt_fail("Foundation bplist: string size overflow");
            bounds(offset, len * (type == 6 ? 2 : 1));
            if (type == 4) { result = rt_new("NSData"); rt_host(result).text = bytes.substr(offset, len); }
            else if (type == 5) result = ns(bytes.substr(offset, len));
            else { std::u16string chars; chars.reserve(len); for (uint64_t i = 0; i < len; ++i) chars += char16_t(read(offset + i * 2, 2)); result = ns(utf8(chars)); }
        } else if (type == 10 || type == 13) {
            uint64_t len = length(info, offset);
            if (len > objects || len > UINT64_MAX / (reference_size * (type == 13 ? 2 : 1))) rt_fail("Foundation bplist: collection size overflow");
            bounds(offset, len * reference_size * (type == 13 ? 2 : 1));
            result = type == 10 ? array() : dictionary();
            for (uint64_t i = 0; i < len; ++i) {
                Obj first = object(read(offset + i * reference_size, reference_size), depth + 1);
                if (type == 10) rt_host(result).array.push_back(hold(first));
                else {
                    Obj value = object(read(offset + (len + i) * reference_size, reference_size), depth + 1);
                    rt_host(result).dictionary[str(first)] = hold(value);
                }
            }
        } else rt_fail("Foundation bplist: unsupported object type");
        active[index] = false; cache[index] = result; return result;
    }
public:
    explicit BinaryPlist(const std::string& input): bytes(input) {
        if (bytes.size() < 40 || bytes.compare(0, 8, "bplist00")) rt_fail("Foundation bplist: bad header");
        size_t trailer = bytes.size() - 32;
        offset_size = static_cast<unsigned char>(bytes[trailer + 6]);
        reference_size = static_cast<unsigned char>(bytes[trailer + 7]);
        objects = read(trailer + 8, 8); top = read(trailer + 16, 8); table = read(trailer + 24, 8);
        if (!offset_size || offset_size > 8 || !reference_size || reference_size > 8 || !objects || objects > 1000000 || top >= objects || table < 8 || table > trailer || objects > (trailer - table) / offset_size) rt_fail("Foundation bplist: invalid trailer");
        cache.resize(objects); active.resize(objects);
    }
    Obj decode() { Obj result=object(top);for(Obj value:cache)if(value&&value!=result)drop(value);return result; }
};

struct XMLToken { enum Kind { Start, End, Text } kind; std::string name, text; std::unordered_map<std::string,std::string> attributes; bool empty = false; };
std::vector<XMLToken> tokenize_xml(const std::string& input) {
    std::vector<XMLToken> result; size_t pos = 0;
    while (pos < input.size()) {
        if (input[pos] != '<') { size_t end = input.find('<', pos); if (end == std::string::npos) end = input.size(); result.push_back({XMLToken::Text, {}, xml_unescape(input.substr(pos,end-pos)),{},false}); pos=end; continue; }
        if (input.compare(pos,4,"<!--")==0) { auto end=input.find("-->",pos+4); if(end==std::string::npos)rt_fail("Foundation XML: unterminated comment");pos=end+3;continue; }
        if (input.compare(pos,9,"<![CDATA[")==0) { auto end=input.find("]]>",pos+9);if(end==std::string::npos)rt_fail("Foundation XML: unterminated CDATA");result.push_back({XMLToken::Text,{},input.substr(pos+9,end-pos-9),{},false});pos=end+3;continue; }
        if (input.compare(pos,2,"<?")==0) { auto end=input.find("?>",pos+2);if(end==std::string::npos)rt_fail("Foundation XML: unterminated declaration");pos=end+2;continue; }
        if (input.compare(pos,9,"<!DOCTYPE")==0) { auto end=input.find('>',pos+9);if(end==std::string::npos||input.substr(pos,end-pos).find('[')!=std::string::npos)rt_fail("Foundation XML: unsupported DTD");pos=end+1;continue; }
        XMLToken token; token.kind=XMLToken::Start; ++pos;
        if (pos<input.size() && input[pos]=='/') { token.kind=XMLToken::End; ++pos; }
        auto skip=[&](){while(pos<input.size()&&std::isspace(static_cast<unsigned char>(input[pos])))++pos;};
        skip();size_t begin=pos;
        while(pos<input.size()&&!std::isspace(static_cast<unsigned char>(input[pos]))&&input[pos]!='/'&&input[pos]!='>')++pos;
        token.name=input.substr(begin,pos-begin);if(token.name.empty())rt_fail("Foundation XML: empty tag");
        for (;;) {
            skip();if(pos>=input.size())rt_fail("Foundation XML: unterminated tag");
            if(input[pos]=='>'){++pos;break;}
            if(input[pos]=='/'&&pos+1<input.size()&&input[pos+1]=='>'){token.empty=true;pos+=2;break;}
            if(token.kind==XMLToken::End)rt_fail("Foundation XML: attributes on end tag");
            begin=pos;while(pos<input.size()&&input[pos]!='='&&!std::isspace(static_cast<unsigned char>(input[pos])))++pos;
            std::string key=input.substr(begin,pos-begin);skip();if(pos>=input.size()||input[pos++]!='=')rt_fail("Foundation XML: missing attribute equals");skip();
            if(pos>=input.size()||(input[pos]!='\''&&input[pos]!='"'))rt_fail("Foundation XML: missing attribute quote");char quote=input[pos++];begin=pos;auto end=input.find(quote,pos);if(end==std::string::npos)rt_fail("Foundation XML: unterminated attribute");
            token.attributes[key]=xml_unescape(input.substr(begin,end-begin));pos=end+1;
        }
        result.push_back(std::move(token));
    }
    return result;
}
Obj parse_plist(const std::string& data) {
    if (data.compare(0,8,"bplist00")==0) return BinaryPlist(data).decode();
    auto tokens=tokenize_xml(data);size_t pos=0;
    auto skip=[&](){while(pos<tokens.size()&&tokens[pos].kind==XMLToken::Text&&tokens[pos].text.find_first_not_of(" \t\r\n")==std::string::npos)++pos;};
    std::function<Obj(unsigned)> parse=[&](unsigned depth)->Obj {
        if(depth>128)rt_fail("Foundation plist XML: excessive nesting");skip();if(pos>=tokens.size()||tokens[pos].kind!=XMLToken::Start)rt_fail("Foundation plist XML: expected object");
        auto token=tokens[pos++];Obj result=nullptr;
        if(token.name=="dict"||token.name=="array") {
            result=token.name=="dict"?dictionary():array();
            if(!token.empty)for(;;){skip();if(pos>=tokens.size())rt_fail("Foundation plist XML: unclosed collection");if(tokens[pos].kind==XMLToken::End)break;
                Obj key=parse(depth+1);if(token.name=="dict"){Obj value=parse(depth+1);rt_host(result).dictionary[str(key)]=value;}else rt_host(result).array.push_back(key);
            }
        } else {
            std::string text;while(!token.empty&&pos<tokens.size()&&tokens[pos].kind==XMLToken::Text)text+=tokens[pos++].text;
            if(token.name=="string"||token.name=="key")result=ns(text);
            else if(token.name=="true"||token.name=="false")result=number(token.name=="true");
            else if(token.name=="integer"||token.name=="real")result=number(std::strtod(text.c_str(),nullptr));
            else rt_fail("Foundation plist XML: unsupported scalar");
        }
        if(!token.empty){skip();if(pos>=tokens.size()||tokens[pos].kind!=XMLToken::End||tokens[pos].name!=token.name)rt_fail("Foundation plist XML: mismatched end tag");++pos;}
        return result;
    };
    skip();if(pos>=tokens.size()||tokens[pos].name!="plist")rt_fail("Foundation plist XML: missing plist root");++pos;return parse(0);
}
Obj plist_file(Obj,Sel,Obj path){bool ok;auto bytes=read_file(str(path),ok);if(!ok)return nullptr;return parse_plist(bytes);}

Obj string_init(Obj self,Sel,Obj value){rt_host(self).text=str(value);return self;}
Obj string_empty(Obj,Sel){return ns("");}
Obj string_capacity_init(Obj self,Sel,uint64_t capacity){rt_host(self).text.clear();rt_host(self).text.reserve(capacity);return self;}
Obj string_capacity_class(Obj,Sel,uint64_t capacity){return string_capacity_init(rt_new("NSMutableString"),nullptr,capacity);}
Obj string_cstr(Obj,Sel,const char* value){return rt_string(value?value:"");}
Obj string_cstr_encoding(Obj,Sel,const char* value,uint64_t encoding){if(encoding!=4&&encoding!=1&&encoding!=5)rt_fail("Foundation NSString: unsupported C string encoding");return rt_string(value?value:"");}
Obj string_bytes(Obj self,Sel,const void* bytes,uint64_t length,uint64_t encoding){
    if(length>SIZE_MAX||(!bytes&&length))rt_fail("Foundation NSString: invalid bytes");
    if(encoding==4||encoding==1||encoding==5)rt_host(self).text.assign(static_cast<const char*>(bytes),length);
    else if(encoding==10||encoding==0x90000100||encoding==0x94000100){if(length%2)rt_fail("Foundation NSString: odd UTF16 bytes");std::u16string text;auto p=static_cast<const unsigned char*>(bytes);bool le=encoding!=0x90000100;for(uint64_t i=0;i<length;i+=2)text+=char16_t(le?(p[i]|p[i+1]<<8):(p[i]<<8|p[i+1]));rt_host(self).text=utf8(text);}
    else rt_fail("Foundation NSString: unsupported byte encoding");return self;
}
const char* string_utf8(Obj self,Sel){return rt_utf8(self);}
const char* string_cstring(Obj self,Sel,uint64_t encoding){if(encoding!=4&&encoding!=1&&encoding!=5)rt_fail("Foundation NSString: unsupported C encoding");return rt_utf8(self);}
uint64_t string_length(Obj self,Sel){return utf16(str(self)).size();}
uint64_t string_byte_length(Obj self,Sel,uint64_t encoding){if(encoding==4||encoding==1||encoding==5)return str(self).size();if(encoding==10)return utf16(str(self)).size()*2;rt_fail("Foundation NSString: unsupported byte length encoding");}
bool string_get_cstr(Obj self,Sel,char* dest,uint64_t length,uint64_t encoding){auto value=str(self);if(encoding!=4&&encoding!=1&&encoding!=5)rt_fail("Foundation NSString: unsupported getCString encoding");if(!dest||length<=value.size())return false;std::memcpy(dest,value.c_str(),value.size()+1);return true;}
bool string_equal(Obj self,Sel,Obj other){return str(self)==str(other);}
int64_t string_compare(Obj self,Sel,Obj other){auto a=str(self),b=str(other);return a<b?-1:a>b?1:0;}
int64_t string_compare_options(Obj self,Sel,Obj other,uint64_t options){auto a=str(self),b=str(other);if(options&1){std::transform(a.begin(),a.end(),a.begin(),::tolower);std::transform(b.begin(),b.end(),b.begin(),::tolower);}if(options&64){std::istringstream as(a),bs(b);unsigned av=0,bv=0;char c;while(as||bs){av=bv=0;as>>av;bs>>bv;if(av!=bv)return av<bv?-1:1;as>>c;bs>>c;}return 0;}return a<b?-1:a>b?1:0;}
uint16_t string_character(Obj self,Sel,uint64_t index){auto text=utf16(str(self));if(index>=text.size())rt_fail("Foundation NSString: character index outside string");return text[index];}
Obj string_substring(Obj self,Sel,Range range){auto text=utf16(str(self));if(range.location>text.size()||range.length>text.size()-range.location)rt_fail("Foundation NSString: substring range outside string");return ns(utf8(text.substr(range.location,range.length)));}
Obj string_from(Obj self,Sel,uint64_t index){auto text=utf16(str(self));if(index>text.size())rt_fail("Foundation NSString: substring start outside string");return ns(utf8(text.substr(index)));}
Obj string_to(Obj self,Sel,uint64_t index){return string_substring(self,nullptr,{0,index});}
Range string_range(Obj self,Sel,Obj needle){auto text=utf16(str(self)),part=utf16(str(needle));auto at=text.find(part);return {at==std::u16string::npos?not_found:at,at==std::u16string::npos?0:part.size()};}
Obj string_append(Obj self,Sel,Obj suffix){return ns(str(self)+str(suffix));}
void string_mutable_append(Obj self,Sel,Obj suffix){rt_host(self).text+=str(suffix);}
void string_set(Obj self,Sel,Obj value){rt_host(self).text=str(value);}
Obj string_lower(Obj self,Sel){auto value=str(self);std::transform(value.begin(),value.end(),value.begin(),::tolower);return ns(value);}
Obj string_upper(Obj self,Sel){auto value=str(self);std::transform(value.begin(),value.end(),value.begin(),::toupper);return ns(value);}
bool string_prefix(Obj self,Sel,Obj prefix){auto value=str(self),p=str(prefix);return value.compare(0,p.size(),p)==0;}
bool string_suffix(Obj self,Sel,Obj suffix){auto value=str(self),s=str(suffix);return s.size()<=value.size()&&value.compare(value.size()-s.size(),s.size(),s)==0;}
bool string_contains(Obj self,Sel,Obj other){return str(self).find(str(other))!=std::string::npos;}
Obj string_replace(Obj self,Sel,Obj from,Obj to){auto value=str(self),f=str(from),t=str(to);if(f.empty())return ns(value);for(size_t pos=0;(pos=value.find(f,pos))!=std::string::npos;pos+=t.size())value.replace(pos,f.size(),t);return ns(value);}
Obj string_components(Obj self,Sel,Obj separator){std::vector<Obj> values;auto text=str(self),sep=str(separator);if(sep.empty())return array({ns(text)});size_t begin=0;for(;;){auto end=text.find(sep,begin);values.push_back(ns(text.substr(begin,end==std::string::npos?end:end-begin)));if(end==std::string::npos)break;begin=end+sep.size();}return array(values);}
Obj string_components_set(Obj self,Sel,Obj set){auto text=utf16(str(self)),chars=utf16(rt_host(set).text);std::vector<Obj> values;size_t begin=0;for(size_t i=0;i<=text.size();++i)if(i==text.size()||chars.find(text[i])!=std::u16string::npos){values.push_back(ns(utf8(text.substr(begin,i-begin))));begin=i+1;}return array(values);}
Obj string_trim(Obj self,Sel,Obj set){auto text=utf16(str(self)),chars=utf16(rt_host(set).text);size_t begin=text.find_first_not_of(chars);if(begin==std::u16string::npos)return ns("");return ns(utf8(text.substr(begin,text.find_last_not_of(chars)-begin+1)));}
Obj string_padding(Obj self,Sel,uint64_t length,Obj padding,uint64_t index){auto text=utf16(str(self)),pad=utf16(str(padding));if(length<=text.size())return ns(utf8(text.substr(0,length)));if(pad.empty()||index>=pad.size())rt_fail("Foundation NSString: invalid padding");while(text.size()<length){text+=pad[index];index=(index+1)%pad.size();}return ns(utf8(text));}
Obj string_copy(Obj self,Sel,void*){return hold(self);}
Obj string_mutable_copy(Obj self,Sel,void*){Obj result=rt_new("NSMutableString");rt_host(result).text=str(self);return result;}
Obj string_path_append(Obj self,Sel,Obj part){auto base=fs::path(str(self)),suffix=fs::path(str(part));if(suffix.is_absolute())suffix=suffix.relative_path();return ns((base/suffix).lexically_normal().string());}
Obj string_path_last(Obj self,Sel){return ns(fs::path(str(self)).filename().string());}
Obj string_path_parent(Obj self,Sel){return ns(fs::path(str(self)).parent_path().string());}
Obj string_path_extension(Obj self,Sel){auto text=fs::path(str(self)).extension().string();return ns(text.empty()?text:text.substr(1));}
Obj string_path_remove_extension(Obj self,Sel){auto path=fs::path(str(self));path.replace_extension();return ns(path.string());}
Obj string_path_add_extension(Obj self,Sel,Obj extension){return ns(str(self)+"."+str(extension));}
Obj string_path_standard(Obj self,Sel){return ns(fs::path(str(self)).lexically_normal().string());}
bool string_path_absolute(Obj self,Sel){return fs::path(str(self)).is_absolute();}
int64_t string_integer(Obj self,Sel){return std::strtoll(rt_utf8(self),nullptr,10);}
double string_double(Obj self,Sel){return std::strtod(rt_utf8(self),nullptr);}
float string_float(Obj self,Sel){return static_cast<float>(string_double(self,nullptr));}
bool string_bool(Obj self,Sel){auto value=str(self);return !value.empty()&&(value[0]=='Y'||value[0]=='y'||value[0]=='T'||value[0]=='t'||std::strtoll(value.c_str(),nullptr,10)!=0);}
Obj string_data(Obj self,Sel,uint64_t encoding){Obj o=rt_new("NSData");if(encoding!=4&&encoding!=1&&encoding!=5)rt_fail("Foundation NSString: unsupported data encoding");rt_host(o).text=str(self);return o;}
Obj string_file(Obj,Sel,Obj path,uint64_t encoding,Obj* error){if(error)*error=nullptr;bool ok;auto text=read_file(str(path),ok);if(!ok)return nullptr;Obj result=rt_new("NSString");return string_bytes(result,nullptr,text.data(),text.size(),encoding);}
Obj string_file_init(Obj self,Sel,Obj path){bool ok;auto text=read_file(str(path),ok);if(!ok)return nullptr;rt_host(self).text=std::move(text);return self;}
uint64_t string_default_encoding(Obj,Sel){return 4;}

Obj number_integer(Obj,Sel,int64_t value){return number(value);}
Obj number_int(Obj,Sel,int32_t value){return number(value);}
Obj number_uint(Obj,Sel,uint32_t value){return number(value);}
Obj number_unsigned(Obj,Sel,uint64_t value){return number(value);}
Obj number_bool(Obj,Sel,bool value){return number(value);}
Obj number_double(Obj,Sel,double value){return number(value);}
Obj number_float(Obj,Sel,float value){return number(value);}
Obj number_init_int(Obj self,Sel,int32_t value){rt_host(self).number=value;return self;}
Obj number_init_integer(Obj self,Sel,int64_t value){rt_host(self).number=value;return self;}
Obj number_init_uint(Obj self,Sel,uint32_t value){rt_host(self).number=value;return self;}
Obj number_init_unsigned(Obj self,Sel,uint64_t value){rt_host(self).number=value;return self;}
Obj number_init_bool(Obj self,Sel,bool value){rt_host(self).number=value;return self;}
Obj number_init_float(Obj self,Sel,float value){rt_host(self).number=value;return self;}
Obj number_init_double(Obj self,Sel,double value){rt_host(self).number=value;return self;}
int64_t number_integer_value(Obj self,Sel){return static_cast<int64_t>(rt_host(self).number);}
uint64_t number_unsigned_value(Obj self,Sel){return static_cast<uint64_t>(rt_host(self).number);}
double number_double_value(Obj self,Sel){return rt_host(self).number;}
float number_float_value(Obj self,Sel){return rt_host(self).number;}
bool number_bool_value(Obj self,Sel){return rt_host(self).number!=0;}
Obj number_string(Obj self,Sel){char buffer[96];std::snprintf(buffer,sizeof buffer,"%.17g",rt_host(self).number);return rt_string(buffer);}

Obj collection_new(Obj self,Sel){return rt_new(rt_class_name(self));}
Obj collection_capacity(Obj self,Sel,uint64_t){return self;}
Obj collection_class_capacity(Obj self,Sel,uint64_t){return rt_new(rt_class_name(self));}
uint64_t array_count(Obj self,Sel){return rt_host(self).array.size();}
Obj array_at(Obj self,Sel,uint64_t index){auto& values=rt_host(self).array;if(index>=values.size())rt_fail("Foundation NSArray: index outside array");return values[index];}
Obj array_first(Obj self,Sel){auto& values=rt_host(self).array;return values.empty()?nullptr:values.front();}
Obj array_last(Obj self,Sel){auto& values=rt_host(self).array;return values.empty()?nullptr:values.back();}
Obj array_one(Obj,Sel,Obj value){if(!value)rt_fail("Foundation NSArray: nil object");return array({value});}
Obj array_counted(Obj,Sel,Obj const* values,uint64_t count){if(count>10000000||(!values&&count))rt_fail("Foundation NSArray: invalid count");return array(std::vector<Obj>(values,values+count));}
Obj array_copy_init(Obj self,Sel,Obj other){auto values=rt_host(other).array;for(Obj value:rt_host(self).array)drop(value);rt_host(self).array.clear();for(Obj value:values)rt_host(self).array.push_back(hold(value));return self;}
Obj array_copy_zone(Obj self,Sel,void*){return array(rt_host(self).array);}
Obj array_copy_class(Obj,Sel,Obj other){return array(rt_host(other).array);}
void array_add(Obj self,Sel,Obj value){if(!value)rt_fail("Foundation NSMutableArray: cannot add nil");rt_host(self).array.push_back(hold(value));changed(self);}
void array_add_array(Obj self,Sel,Obj other){auto copy=rt_host(other).array;for(Obj value:copy)array_add(self,nullptr,value);}
bool equal_objects(Obj a,Obj b){if(a==b)return true;if(!a||!b)return false;return send<bool>(a,"isEqual:",b);}
uint64_t array_index(Obj self,Sel,Obj value){auto& values=rt_host(self).array;for(size_t i=0;i<values.size();++i)if(equal_objects(values[i],value))return i;return not_found;}
bool array_contains(Obj self,Sel,Obj value){return array_index(self,nullptr,value)!=not_found;}
void array_remove_index(Obj self,Sel,uint64_t index){auto& values=rt_host(self).array;if(index>=values.size())rt_fail("Foundation NSMutableArray: removal index outside array");Obj old=values[index];values.erase(values.begin()+index);changed(self);drop(old);}
void array_remove(Obj self,Sel,Obj value){for(size_t i=rt_host(self).array.size();i>0;--i)if(equal_objects(rt_host(self).array[i-1],value))array_remove_index(self,nullptr,i-1);}
void array_remove_identical(Obj self,Sel,Obj value){for(size_t i=rt_host(self).array.size();i>0;--i)if(rt_host(self).array[i-1]==value)array_remove_index(self,nullptr,i-1);}
void array_clear(Obj self,Sel){auto values=std::move(rt_host(self).array);rt_host(self).array.clear();changed(self);for(Obj value:values)drop(value);}
void array_remove_last(Obj self,Sel){auto count=rt_host(self).array.size();if(!count)rt_fail("Foundation NSMutableArray: removing from empty array");array_remove_index(self,nullptr,count-1);}
void array_insert(Obj self,Sel,Obj value,uint64_t index){auto& values=rt_host(self).array;if(index>values.size()||!value)rt_fail("Foundation NSMutableArray: invalid insert");values.insert(values.begin()+index,hold(value));changed(self);}
void array_replace(Obj self,Sel,uint64_t index,Obj value){auto& values=rt_host(self).array;if(index>=values.size()||!value)rt_fail("Foundation NSMutableArray: invalid replacement");Obj old=values[index];values[index]=hold(value);changed(self);drop(old);}
void array_exchange(Obj self,Sel,uint64_t a,uint64_t b){auto& values=rt_host(self).array;if(a>=values.size()||b>=values.size())rt_fail("Foundation NSMutableArray: exchange outside array");std::swap(values[a],values[b]);changed(self);}
using ArrayComparator = int64_t (*)(Obj,Obj,void*);
void array_sort_function(Obj self,Sel,ArrayComparator compare,void* context){
    if(!compare)rt_fail("Foundation NSMutableArray: nil sort comparator");
    auto& values=rt_host(self).array;
    // Darwin LP64 NSInteger is signed 64-bit; callbacks receive the objects
    // themselves. Neither comparator nor context escapes this synchronous call.
    std::stable_sort(values.begin(),values.end(),[compare,context](Obj a,Obj b){return compare(a,b,context)<0;});
    changed(self);
}
Obj array_join(Obj self,Sel,Obj separator){std::string result,sep=str(separator);bool first=true;for(Obj value:rt_host(self).array){if(!first)result+=sep;first=false;result+=str(value);}return ns(result);}
Obj array_enumerator(Obj self,Sel){Obj result=rt_new("NSEnumerator");for(Obj value:rt_host(self).array)rt_host(result).array.push_back(hold(value));return result;}
Obj array_reverse_enumerator(Obj self,Sel){Obj result=array_enumerator(self,nullptr);std::reverse(rt_host(result).array.begin(),rt_host(result).array.end());return result;}
Obj enumerator_next(Obj self,Sel){auto& values=rt_host(self).array;auto& index=enumeration_position[self];return index<values.size()?values[index++]:nullptr;}
uint64_t enumerator_enumerate(Obj self,Sel,EnumerationState* state,Obj* buffer,uint64_t length){
    if(!state||(!buffer&&length))rt_fail("Foundation NSEnumerator: invalid fast enumeration");
    auto& values=rt_host(self).array;auto& index=enumeration_position[self];
    if(!length||index>=values.size())return 0;
    uint64_t count=std::min<uint64_t>(length,values.size()-index);
    for(uint64_t i=0;i<count;++i)buffer[i]=values[index+i];
    // nextObject and every fast-enumeration state share this consumable cursor.
    // A returned batch is consumed now; its objects remain owned by the snapshot.
    index+=count;state->state=index;state->items=buffer;state->mutations=&mutation_words[self];return count;
}
uint64_t array_enumerate(Obj self,Sel,EnumerationState* state,Obj* buffer,uint64_t length){auto& values=rt_host(self).array;if(!state||(!buffer&&length))rt_fail("Foundation: invalid fast enumeration");size_t start=state->state;if(start>=values.size())return 0;size_t count=std::min<uint64_t>(length,values.size()-start);for(size_t i=0;i<count;++i)buffer[i]=values[start+i];state->items=buffer;state->mutations=&mutation_words[self];state->state+=count;return count;}
void array_perform(Obj self,Sel,Sel action){auto values=rt_host(self).array;for(Obj value:values)send<void>(value,action);}
void array_perform_object(Obj self,Sel,Sel action,Obj argument){auto values=rt_host(self).array;for(Obj value:values)send<void>(value,action,argument);}

uint64_t dict_count(Obj self,Sel){return rt_host(self).dictionary.size();}
Obj dict_get(Obj self,Sel,Obj key){if(!key)return nullptr;auto& values=rt_host(self).dictionary;auto found=values.find(str(key));return found==values.end()?nullptr:found->second;}
void dict_set(Obj self,Sel,Obj value,Obj key){if(!key||!value)rt_fail("Foundation NSMutableDictionary: nil key/value");auto& slot=rt_host(self).dictionary[str(key)];Obj old=slot;slot=hold(value);changed(self);drop(old);}
void dict_remove(Obj self,Sel,Obj key){auto& values=rt_host(self).dictionary;auto it=values.find(str(key));if(it!=values.end()){Obj old=it->second;values.erase(it);changed(self);drop(old);}}
void dict_clear(Obj self,Sel){auto values=std::move(rt_host(self).dictionary);rt_host(self).dictionary.clear();changed(self);for(auto& pair:values)drop(pair.second);}
Obj dict_keys(Obj self,Sel){std::vector<Obj> values;for(auto& pair:rt_host(self).dictionary)values.push_back(ns(pair.first));return array(values);}
Obj dict_values(Obj self,Sel){std::vector<Obj> values;for(auto& pair:rt_host(self).dictionary)values.push_back(pair.second);return array(values);}
Obj dict_one(Obj,Sel,Obj value,Obj key){Obj result=dictionary();dict_set(result,nullptr,value,key);return result;}
Obj dict_arrays(Obj,Sel,Obj values,Obj keys){auto& vs=rt_host(values).array;auto& ks=rt_host(keys).array;if(vs.size()!=ks.size())rt_fail("Foundation NSDictionary: mismatched key/value arrays");Obj result=dictionary();for(size_t i=0;i<vs.size();++i)dict_set(result,nullptr,vs[i],ks[i]);return result;}
Obj dict_copy_init(Obj self,Sel,Obj other){auto values=rt_host(other).dictionary;dict_clear(self,nullptr);for(auto& pair:values)rt_host(self).dictionary[pair.first]=hold(pair.second);return self;}
Obj dict_copy_class(Obj,Sel,Obj other){Obj result=dictionary();for(auto& pair:rt_host(other).dictionary)rt_host(result).dictionary[pair.first]=hold(pair.second);return result;}
Obj dict_copy_zone(Obj self,Sel,void*){return dict_copy_class(nullptr,nullptr,self);}
Obj dict_file_init(Obj self,Sel,Obj path){Obj parsed=plist_file(nullptr,nullptr,path);if(!parsed)return nullptr;dict_copy_init(self,nullptr,parsed);drop(parsed);return self;}
void dict_add_dictionary(Obj self,Sel,Obj other){auto values=rt_host(other).dictionary;for(auto& item:values)dict_set(self,nullptr,item.second,ns(item.first));}
Obj dict_key_enumerator(Obj self,Sel){return array_enumerator(dict_keys(self,nullptr),nullptr);}
Obj dict_value_enumerator(Obj self,Sel){return array_enumerator(dict_values(self,nullptr),nullptr);}
uint64_t dict_enumerate(Obj self,Sel,EnumerationState* state,Obj* buffer,uint64_t length){if(state->state==0)state->extra[0]=reinterpret_cast<unsigned long>(dict_keys(self,nullptr));auto count=array_enumerate(reinterpret_cast<Obj>(state->extra[0]),nullptr,state,buffer,length);state->mutations=&mutation_words[self];if(!count&&state->extra[0]){drop(reinterpret_cast<Obj>(state->extra[0]));state->extra[0]=0;}return count;}
void set_add(Obj self,Sel,Obj value){if(!array_contains(self,nullptr,value))array_add(self,nullptr,value);}
Obj set_member(Obj self,Sel,Obj value){auto index=array_index(self,nullptr,value);return index==not_found?nullptr:rt_host(self).array[index];}
Obj set_one(Obj,Sel,Obj value){Obj result=rt_new("NSMutableSet");set_add(result,nullptr,value);return result;}
Obj set_all_objects(Obj self,Sel){return array(rt_host(self).array);}
Obj set_copy_zone(Obj self,Sel,void*){Obj result=rt_new("NSMutableSet");for(Obj value:rt_host(self).array)array_add(result,nullptr,value);return result;}

Obj bundle_main(Obj,Sel){return singleton("NSBundle");}
Obj bundle_path(Obj,Sel){return ns(rt_data_root);}
Obj bundle_info(Obj,Sel){static Obj info=nullptr;if(!info){bool ok;auto bytes=read_file(rt_data_root+"/Info.plist",ok);info=ok?parse_plist(bytes):dictionary();}return info;}
Obj bundle_info_key(Obj self,Sel,Obj key){return dict_get(bundle_info(self,nullptr),nullptr,key);}
Obj bundle_resource(Obj,Sel,Obj name,Obj extension){fs::path path=fs::path(rt_data_root)/str(name);if(extension&&!str(extension).empty())path+="."+str(extension);std::error_code ec;return fs::exists(path,ec)?ns(path.string()):nullptr;}
Obj bundle_resource_directory(Obj,Sel,Obj name,Obj extension,Obj directory){fs::path path=fs::path(rt_data_root)/str(directory)/str(name);if(extension&&!str(extension).empty())path+="."+str(extension);std::error_code ec;return fs::exists(path,ec)?ns(path.string()):nullptr;}
Obj bundle_localized(Obj,Sel,Obj key,Obj value,Obj){return value&&!str(value).empty()?value:key;}
Obj file_manager(Obj,Sel){return singleton("NSFileManager");}
bool file_exists(Obj,Sel,Obj path){std::error_code ec;return fs::exists(str(path),ec);}
bool file_exists_dir(Obj,Sel,Obj path,bool* is_dir){std::error_code ec;bool result=fs::exists(str(path),ec);if(is_dir)*is_dir=result&&fs::is_directory(str(path),ec);return result;}
bool file_create_directory(Obj,Sel,Obj path,bool recursive,Obj,Obj* error){if(error)*error=nullptr;std::error_code ec;bool made=recursive?fs::create_directories(str(path),ec):fs::create_directory(str(path),ec);return made||(!ec&&fs::is_directory(str(path),ec));}
Obj file_contents(Obj,Sel,Obj path){return read_data(nullptr,nullptr,path);}
Obj file_directory_contents(Obj,Sel,Obj path,Obj* error){if(error)*error=nullptr;std::error_code ec;std::vector<Obj> entries;fs::directory_iterator it(str(path),ec);if(ec)return nullptr;for(auto& entry:it)entries.push_back(ns(entry.path().filename().string()));return array(entries);}
Obj file_current_directory(Obj,Sel){return ns(rt_data_root);}
bool file_write_data(Obj self,Sel,Obj path,bool atomic){auto target=fs::path(str(path));auto temp=target;if(atomic)temp+=".ipa-study-writing";std::ofstream out(temp,std::ios::binary);if(!out)return false;auto& data=rt_host(self).text;out.write(data.data(),data.size());out.close();if(!out)return false;if(atomic){std::error_code ec;fs::rename(temp,target,ec);return !ec;}return true;}

fs::path defaults_path(){return fs::path(rt_data_root)/".ipa-study/Library/Preferences/com.gelatogames.goblinsword.plist";}
Obj defaults_standard(Obj,Sel){
    static bool loaded=false;Obj result=singleton("NSUserDefaults");
    if(!loaded){loaded=true;bool ok;auto bytes=read_file(defaults_path().string(),ok);if(ok){Obj values=parse_plist(bytes);dict_copy_init(result,nullptr,values);drop(values);}}
    return result;
}
Obj defaults_object(Obj self,Sel,Obj key){return dict_get(self,nullptr,key);}
bool defaults_bool(Obj self,Sel,Obj key){Obj value=dict_get(self,nullptr,key);return value?send<bool>(value,"boolValue"):false;}
int64_t defaults_integer(Obj self,Sel,Obj key){Obj value=dict_get(self,nullptr,key);return value?send<int64_t>(value,"integerValue"):0;}
double defaults_double(Obj self,Sel,Obj key){Obj value=dict_get(self,nullptr,key);return value?send<double>(value,"doubleValue"):0;}
void defaults_set_bool(Obj self,Sel,bool value,Obj key){dict_set(self,nullptr,number(value),key);}
void defaults_set_integer(Obj self,Sel,int64_t value,Obj key){dict_set(self,nullptr,number(value),key);}
void defaults_set_double(Obj self,Sel,double value,Obj key){dict_set(self,nullptr,number(value),key);}
std::string xml_escape(const std::string& text){std::string result;for(char c:text){switch(c){case '&':result+="&amp;";break;case '<':result+="&lt;";break;case '>':result+="&gt;";break;default:result+=c;}}return result;}
void plist_write_object(std::ostream& out,Obj object,unsigned depth=0){
    if(depth>128)rt_fail("Foundation defaults: excessive nesting");std::string cls=rt_class_name(object);
    if(cls=="NSNumber"){out<<"<real>";out.precision(17);out<<rt_host(object).number<<"</real>";}
    else if(cls.find("String")!=std::string::npos)out<<"<string>"<<xml_escape(str(object))<<"</string>";
    else if(cls=="NSArray"||cls=="NSMutableArray"){out<<"<array>";for(Obj item:rt_host(object).array)plist_write_object(out,item,depth+1);out<<"</array>";}
    else if(cls=="NSDictionary"||cls=="NSMutableDictionary"||cls=="NSUserDefaults"){out<<"<dict>";for(auto& pair:rt_host(object).dictionary){out<<"<key>"<<xml_escape(pair.first)<<"</key>";plist_write_object(out,pair.second,depth+1);}out<<"</dict>";}
    else rt_fail("Foundation defaults: unsupported value class");
}
bool defaults_sync(Obj self,Sel){
    fs::path target=defaults_path();std::error_code ec;fs::create_directories(target.parent_path(),ec);if(ec)return false;
    fs::path temp=target;temp+=".writing";std::ofstream out(temp,std::ios::binary);if(!out)return false;
    out<<"<?xml version=\"1.0\" encoding=\"UTF-8\"?><plist version=\"1.0\">";plist_write_object(out,self);out<<"</plist>\n";out.close();if(!out)return false;fs::rename(temp,target,ec);return !ec;
}
void defaults_register(Obj self,Sel,Obj values){for(auto& pair:rt_host(values).dictionary)if(!rt_host(self).dictionary.count(pair.first))rt_host(self).dictionary[pair.first]=hold(pair.second);}

// iCloud is unavailable on this host. This process-local cache is deliberately
// separate from NSUserDefaults and never claims a remote synchronization.
Obj ubiquitous_default(Obj,Sel){return singleton("NSUbiquitousKeyValueStore");}
void ubiquitous_set_object(Obj self,Sel,Obj value,Obj key){if(!key)rt_fail("Foundation iCloud cache: nil key");if(value)dict_set(self,nullptr,value,key);else dict_remove(self,nullptr,key);}
void ubiquitous_set_number(Obj self,double value,Obj key){Obj boxed=number(value);dict_set(self,nullptr,boxed,key);drop(boxed);}
void ubiquitous_set_bool(Obj self,Sel,bool value,Obj key){ubiquitous_set_number(self,value,key);}
void ubiquitous_set_double(Obj self,Sel,double value,Obj key){ubiquitous_set_number(self,value,key);}
Obj ubiquitous_dictionary(Obj self,Sel){return dict_copy_init(rt_new("NSDictionary"),nullptr,self);}
bool ubiquitous_sync(Obj,Sel){std::fprintf(stderr,"[iCloud] NSUbiquitousKeyValueStore unavailable; synchronize=false; cache is local to this process\n");return false;}

Obj notification_center(Obj,Sel){return singleton("NSNotificationCenter");}
void notification_observe(Obj self,Sel,Obj target,Sel selector,Obj name,Obj object){observers[self].push_back({target,selector,str(name),object});}
void notification_remove(Obj self,Sel,Obj target){auto& values=observers[self];values.erase(std::remove_if(values.begin(),values.end(),[target](const Observer& o){return o.target==target;}),values.end());}
void notification_remove_named(Obj self,Sel,Obj target,Obj name,Obj object){auto& values=observers[self];auto text=str(name);values.erase(std::remove_if(values.begin(),values.end(),[=](const Observer& o){return o.target==target&&(!name||o.name==text)&&(!object||o.object==object);}),values.end());}
void notification_post(Obj self,Sel,Obj name,Obj object,Obj info){Obj note=rt_new("NSNotification");rt_host(note).text=str(name);rt_host(note).target=object;rt_host(note).native=info;auto values=observers[self];for(auto& o:values)if((o.name.empty()||o.name==str(name))&&(!o.object||o.object==object))send<void>(o.target,o.selector,note);}
void notification_post_simple(Obj self,Sel,Obj name,Obj object){notification_post(self,nullptr,name,object,nullptr);}
Obj notification_name(Obj self,Sel){return ns(rt_host(self).text);}
Obj notification_object(Obj self,Sel){return rt_host(self).target;}
Obj notification_info(Obj self,Sel){return rt_host(self).native;}
Obj current_thread(Obj,Sel){return singleton("NSThread");}
Obj current_runloop(Obj,Sel){return singleton("NSRunLoop");}
bool thread_main(Obj,Sel){return true;}
void lock_lock(Obj self,Sel){auto& mutex=locks[self];if(!mutex)mutex=std::make_unique<std::recursive_mutex>();mutex->lock();}
void lock_unlock(Obj self,Sel){auto found=locks.find(self);if(found==locks.end())rt_fail("Foundation NSLock: unlock before lock");found->second->unlock();}
bool lock_try(Obj self,Sel){auto& mutex=locks[self];if(!mutex)mutex=std::make_unique<std::recursive_mutex>();return mutex->try_lock();}

Obj data_bytes_init(Obj self,Sel,const void* bytes,uint64_t length){if((!bytes&&length)||length>SIZE_MAX)rt_fail("Foundation NSData: invalid bytes");rt_host(self).text.assign(static_cast<const char*>(bytes),length);return self;}
Obj data_bytes_class(Obj,Sel,const void* bytes,uint64_t length){return data_bytes_init(rt_new("NSData"),nullptr,bytes,length);}
const void* data_bytes(Obj self,Sel){return rt_host(self).text.data();}
void* data_mutable_bytes(Obj self,Sel){return rt_host(self).text.data();}
uint64_t data_length(Obj self,Sel){return rt_host(self).text.size();}
Obj data_init_file(Obj self,Sel,Obj path){bool ok;auto data=read_file(str(path),ok);if(!ok)return nullptr;rt_host(self).text=std::move(data);return self;}
Obj data_new(Obj,Sel){return rt_new("NSMutableData");}
Obj data_length_class(Obj,Sel,uint64_t size){Obj result=rt_new("NSMutableData");rt_host(result).text.resize(size);return result;}
void data_set_length(Obj self,Sel,uint64_t size){rt_host(self).text.resize(size);}
void data_append(Obj self,Sel,const void* bytes,uint64_t size){if(!bytes&&size)rt_fail("Foundation NSMutableData: nil bytes");rt_host(self).text.append(static_cast<const char*>(bytes),size);}
Obj url_path(Obj self,Sel){auto value=rt_host(self).text;if(value.compare(0,7,"file://")==0)value.erase(0,7);return ns(value);}
Obj url_new(Obj,Sel,Obj value){Obj result=rt_new("NSURL");rt_host(result).text=str(value);return result;}
Obj url_init(Obj self,Sel,Obj value){rt_host(self).text=str(value);return self;}
bool url_file(Obj self,Sel){auto value=rt_host(self).text;return value.compare(0,7,"file://")==0||value.find("://")==std::string::npos;}
Obj data_url(Obj,Sel,Obj url){if(!url_file(url,nullptr))rt_fail("Foundation NSData: network URL unsupported");return read_data(nullptr,nullptr,url_path(url,nullptr));}
Obj data_init_url(Obj self,Sel,Obj url){if(!url_file(url,nullptr))rt_fail("Foundation NSData: network URL unsupported");return data_init_file(self,nullptr,url_path(url,nullptr));}
Obj character_set(Obj,Sel,Obj text){Obj result=rt_new("NSCharacterSet");rt_host(result).text=str(text);return result;}
std::u16string whitespace_characters(){return u"\t \u00a0\u1680\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200a\u202f\u205f\u3000";}
std::u16string newline_characters(){return u"\n\r\v\f\u0085\u2028\u2029";}
Obj character_set_whitespace(Obj,Sel){return character_set(nullptr,nullptr,ns(utf8(whitespace_characters())));}
Obj character_set_newline(Obj,Sel){return character_set(nullptr,nullptr,ns(utf8(newline_characters())));}
Obj character_set_whitespace_newline(Obj,Sel){return character_set(nullptr,nullptr,ns(utf8(whitespace_characters()+newline_characters())));}
bool character_set_member(Obj self,Sel,uint16_t value){return utf16(rt_host(self).text).find(char16_t(value))!=std::u16string::npos;}

Obj xml_init_data(Obj self,Sel,Obj data){rt_host(self).text=rt_host(data).text;return self;}
Obj xml_init_url(Obj self,Sel,Obj url){Obj data=data_url(nullptr,nullptr,url);if(!data)return nullptr;return xml_init_data(self,nullptr,data);}
void set_delegate(Obj self,Sel,Obj delegate){rt_host(self).target=delegate;}
Obj get_delegate(Obj self,Sel){return rt_host(self).target;}
void xml_config(Obj,Sel,bool){}
bool xml_parse(Obj self,Sel){
    Obj delegate=rt_host(self).target;if(!delegate)rt_fail("Foundation NSXMLParser: missing delegate");
    auto tokens=tokenize_xml(rt_host(self).text);std::vector<std::string> stack;
    auto responds=[&](Sel selector){return send<bool>(delegate,"respondsToSelector:",selector);};
    if(responds("parserDidStartDocument:"))send<void>(delegate,"parserDidStartDocument:",self);
    for(auto& token:tokens){
        if(token.kind==XMLToken::Text){if(!token.text.empty()&&responds("parser:foundCharacters:"))send<void>(delegate,"parser:foundCharacters:",self,ns(token.text));continue;}
        if(token.kind==XMLToken::Start){Obj attrs=dictionary();for(auto& attr:token.attributes)rt_host(attrs).dictionary[attr.first]=ns(attr.second);
            if(responds("parser:didStartElement:namespaceURI:qualifiedName:attributes:"))send<void>(delegate,"parser:didStartElement:namespaceURI:qualifiedName:attributes:",self,ns(token.name),Obj(nullptr),Obj(nullptr),attrs);
            if(!token.empty){stack.push_back(token.name);continue;}
        }else{if(stack.empty()||stack.back()!=token.name)rt_fail("Foundation NSXMLParser: mismatched closing tag");stack.pop_back();}
        if(responds("parser:didEndElement:namespaceURI:qualifiedName:"))send<void>(delegate,"parser:didEndElement:namespaceURI:qualifiedName:",self,ns(token.name),Obj(nullptr),Obj(nullptr));
    }
    if(!stack.empty())rt_fail("Foundation NSXMLParser: unclosed document");
    if(responds("parserDidEndDocument:"))send<void>(delegate,"parserDidEndDocument:",self);
    return true;
}
Obj process_info(Obj,Sel){return singleton("NSProcessInfo");}
uint64_t processor_count(Obj,Sel){return 1;}
Obj locale_current(Obj,Sel){return singleton("NSLocale");}
Obj locale_identifier(Obj,Sel){return ns("en_US");}
Obj locale_languages(Obj,Sel){return array({ns("en-US"),ns("en")});}
Obj device_current(Obj,Sel){return singleton("UIDevice");}
Obj device_system_version(Obj,Sel){return ns("7.0");}
Obj device_system_name(Obj,Sel){return ns("NextOS iOS compatibility runtime");}
Obj device_name(Obj,Sel){return ns("NextOS IPA study host");}
Obj device_model(Obj,Sel){return ns("NextOS compatibility host");}
int64_t device_idiom(Obj,Sel){return 0;}
int64_t device_orientation(Obj,Sel){return 3;}
void device_begin_orientation(Obj self,Sel){rt_host(self).number=1;}
void device_end_orientation(Obj self,Sel){rt_host(self).number=0;}
bool device_orientation_active(Obj self,Sel){return rt_host(self).number!=0;}
Obj error_new(Obj,Sel,Obj domain,int64_t code,Obj info){Obj result=rt_new("NSError");rt_host(result).text=str(domain);rt_host(result).number=code;rt_host(result).native=info;return result;}
Obj error_domain(Obj self,Sel){return ns(rt_host(self).text);}
int64_t error_code(Obj self,Sel){return static_cast<int64_t>(rt_host(self).number);}
Obj error_info(Obj self,Sel){return rt_host(self).native;}
Obj error_description(Obj self,Sel){Obj info=rt_host(self).native;Obj desc=info?dict_get(info,nullptr,ns("NSLocalizedDescription")):nullptr;if(desc)return desc;return ns(rt_host(self).text+" ("+std::to_string(error_code(self,nullptr))+")");}

Obj method_signature(Obj self,Sel,Sel selector){
    if(!self||!selector)return nullptr;
    const char* types=rt_method_types(self,selector);
    if(!types){if(rt_responds(self,selector))rt_fail("Foundation method signature: method has no registered type encoding");return nullptr;}
    Obj result=rt_new("NSMethodSignature");rt_host(result).text=types;return send<Obj>(result,"autorelease");
}
struct Invocation {Obj signature=nullptr,target=nullptr,argument=nullptr;Sel selector=nullptr;bool argument_set=false;};
std::unordered_map<Obj,Invocation> invocations;
Invocation& invocation_data(Obj self){auto i=invocations.find(self);if(i==invocations.end())rt_fail("Foundation NSInvocation: uninitialized invocation");return i->second;}
Obj invocation_create(Obj,Sel,Obj signature){
    if(!signature||std::strcmp(rt_class_name(signature),"NSMethodSignature"))rt_fail("Foundation NSInvocation: invalid signature object");
    // Observed CCSpecialMenuItem ABI: void callback(id self, SEL, id sender).
    // Preserve all original signature encodings; execute only this proven shape.
    if(rt_host(signature).text!="v24@0:8@16")rt_fail("Foundation NSInvocation: unsupported original method type encoding");
    Obj result=rt_new("NSInvocation");invocations[result].signature=hold(signature);return send<Obj>(result,"autorelease");
}
void invocation_target(Obj self,Sel,Obj target){invocation_data(self).target=target;}
void invocation_selector(Obj self,Sel,Sel selector){invocation_data(self).selector=selector;}
void invocation_argument(Obj self,Sel,const void* value,uint64_t index){
    if(!value||index>2)rt_fail("Foundation NSInvocation: invalid argument address/index");
    auto& call=invocation_data(self);
    if(index==0)std::memcpy(&call.target,value,sizeof(Obj));
    else if(index==1)std::memcpy(&call.selector,value,sizeof(Sel));
    else {std::memcpy(&call.argument,value,sizeof(Obj));call.argument_set=true;}
}
void invocation_invoke(Obj self,Sel){
    auto call=invocation_data(self);if(!call.target)return;
    if(!call.selector||!call.argument_set)rt_fail("Foundation NSInvocation: selector/argument not set");
    const char* actual=rt_method_types(call.target,call.selector);
    if(!actual||rt_host(call.signature).text!=actual)rt_fail("Foundation NSInvocation: target method does not match captured signature");
    send<void>(call.target,call.selector,call.argument);
}

template<class F> void install(std::initializer_list<const char*> classes,const char* selector,F function,bool class_method=false){for(auto name:classes)rt_method(name,selector,reinterpret_cast<void*>(+function),class_method);}
}

extern "C" Obj foundation_string_format_stack(Obj receiver,Sel selector,Obj format,const uint64_t* arguments){
    std::string result,text=str(format);size_t argument=0;
    for(size_t pos=0;pos<text.size();){if(text[pos]!='%'){result+=text[pos++];continue;}size_t begin=pos++;if(pos<text.size()&&text[pos]=='%'){result+='%';++pos;continue;}
        while(pos<text.size()&&std::strchr("-+ #0.123456789hlzjtL",text[pos]))++pos;
        if(pos>=text.size())rt_fail("Foundation NSString format: incomplete conversion");char type=text[pos++];std::string spec=text.substr(begin,pos-begin);uint64_t value=arguments[argument++];char buffer[4096];int count=0;
        if(type=='@'){result+=str(reinterpret_cast<Obj>(value));continue;}
        if(type=='C'){result+=utf8(std::u16string(1,char16_t(value)));continue;}
        if(type=='s')count=std::snprintf(buffer,sizeof buffer,spec.c_str(),reinterpret_cast<const char*>(value));
        else if(type=='d'||type=='i')count=std::snprintf(buffer,sizeof buffer,spec.c_str(),static_cast<int64_t>(value));
        else if(type=='u'||type=='x'||type=='X'||type=='o')count=std::snprintf(buffer,sizeof buffer,spec.c_str(),value);
        else if(type=='c')count=std::snprintf(buffer,sizeof buffer,spec.c_str(),static_cast<int>(value));
        else if(type=='p')count=std::snprintf(buffer,sizeof buffer,spec.c_str(),reinterpret_cast<void*>(value));
        else if(type=='f'||type=='g'||type=='e'||type=='E'||type=='G'){double floating;std::memcpy(&floating,&value,8);count=std::snprintf(buffer,sizeof buffer,spec.c_str(),floating);}
        else rt_fail("Foundation NSString format: unsupported conversion");
        if(count<0||size_t(count)>=sizeof buffer)rt_fail("Foundation NSString format: formatting failed/oversized");result.append(buffer,count);
    }
    if(std::strcmp(selector,"initWithFormat:")==0){rt_host(receiver).text=result;return receiver;}
    if(std::strcmp(selector,"stringByAppendingFormat:")==0)return ns(str(receiver)+result);
    return ns(result);
}
extern "C" Obj foundation_array_objects_stack(Obj receiver,Sel selector,Obj first,Obj const* arguments){
    Obj result=std::strncmp(selector,"init",4)==0?receiver:array();array_clear(result,nullptr);
    Obj value=first;for(size_t i=0;value;++i){if(i>1000000)rt_fail("Foundation NSArray: unterminated variadic list");array_add(result,nullptr,value);value=arguments[i];}return result;
}
extern "C" Obj foundation_dictionary_objects_keys_stack(Obj receiver,Sel selector,Obj first,Obj const* arguments){
    Obj result=std::strncmp(selector,"init",4)==0?receiver:dictionary();dict_clear(result,nullptr);
    Obj value=first;for(size_t i=0;value;i+=2){if(i>2000000)rt_fail("Foundation NSDictionary: unterminated variadic list");Obj key=arguments[i];if(!key)rt_fail("Foundation NSDictionary: missing variadic key");dict_set(result,nullptr,value,key);value=arguments[i+1];}return result;
}
extern "C" void foundation_string_format();
extern "C" void foundation_array_objects();
extern "C" void foundation_dictionary_objects_keys();

void rt_install_foundation(){
    install({"NSObject"},"methodSignatureForSelector:",method_signature);install({"NSObject"},"methodSignatureForSelector:",method_signature,true);
    install({"NSInvocation"},"invocationWithMethodSignature:",invocation_create,true);install({"NSInvocation"},"setTarget:",invocation_target);install({"NSInvocation"},"setSelector:",invocation_selector);install({"NSInvocation"},"setArgument:atIndex:",invocation_argument);install({"NSInvocation"},"invoke",invocation_invoke);
    auto strings={"NSString","NSMutableString","NSConstantString","__NSCFConstantString"};
    install(strings,"initWithString:",string_init);install(strings,"stringWithString:",[](Obj,Sel,Obj v)->Obj{return ns(str(v));},true);
    install(strings,"string",string_empty,true);install({"NSMutableString"},"initWithCapacity:",string_capacity_init);install({"NSMutableString"},"stringWithCapacity:",string_capacity_class,true);
    install(strings,"stringWithUTF8String:",string_cstr,true);install(strings,"stringWithCString:encoding:",string_cstr_encoding,true);
    install(strings,"initWithUTF8String:",[](Obj self,Sel,const char* v)->Obj{rt_host(self).text=v?v:"";return self;});
    install(strings,"initWithBytes:length:encoding:",string_bytes);install(strings,"UTF8String",string_utf8);install(strings,"cStringUsingEncoding:",string_cstring);
    install(strings,"length",string_length);install(strings,"lengthOfBytesUsingEncoding:",string_byte_length);install(strings,"getCString:maxLength:encoding:",string_get_cstr);
    install(strings,"isEqualToString:",string_equal);install(strings,"isEqual:",string_equal);install(strings,"compare:",string_compare);install(strings,"compare:options:",string_compare_options);
    install(strings,"characterAtIndex:",string_character);install(strings,"substringWithRange:",string_substring);install(strings,"substringFromIndex:",string_from);install(strings,"substringToIndex:",string_to);install(strings,"rangeOfString:",string_range);
    install(strings,"stringByAppendingString:",string_append);install(strings,"appendString:",string_mutable_append);install(strings,"setString:",string_set);
    install(strings,"lowercaseString",string_lower);install(strings,"uppercaseString",string_upper);install(strings,"hasPrefix:",string_prefix);install(strings,"hasSuffix:",string_suffix);install(strings,"containsString:",string_contains);
    install(strings,"stringByReplacingOccurrencesOfString:withString:",string_replace);install(strings,"componentsSeparatedByString:",string_components);install(strings,"componentsSeparatedByCharactersInSet:",string_components_set);install(strings,"stringByTrimmingCharactersInSet:",string_trim);
    install(strings,"stringByAppendingPathComponent:",string_path_append);install(strings,"lastPathComponent",string_path_last);install(strings,"stringByDeletingLastPathComponent",string_path_parent);install(strings,"pathExtension",string_path_extension);install(strings,"stringByDeletingPathExtension",string_path_remove_extension);install(strings,"stringByAppendingPathExtension:",string_path_add_extension);install(strings,"stringByStandardizingPath",string_path_standard);install(strings,"isAbsolutePath",string_path_absolute);
    install(strings,"intValue",string_integer);install(strings,"integerValue",string_integer);install(strings,"longLongValue",string_integer);install(strings,"doubleValue",string_double);install(strings,"floatValue",string_float);install(strings,"boolValue",string_bool);install(strings,"dataUsingEncoding:",string_data);
    install(strings,"stringWithContentsOfFile:encoding:error:",string_file,true);install(strings,"initWithContentsOfFile:",string_file_init);install(strings,"defaultCStringEncoding",string_default_encoding,true);
    install(strings,"stringWithFormat:",foundation_string_format,true);install(strings,"initWithFormat:",foundation_string_format);
    install(strings,"stringByAppendingFormat:",foundation_string_format);
    install(strings,"stringByPaddingToLength:withString:startingAtIndex:",string_padding);install(strings,"copyWithZone:",string_copy);install(strings,"mutableCopyWithZone:",string_mutable_copy);

    install({"NSNumber"},"numberWithBool:",number_bool,true);install({"NSNumber"},"numberWithInt:",number_int,true);install({"NSNumber"},"numberWithInteger:",number_integer,true);install({"NSNumber"},"numberWithLongLong:",number_integer,true);install({"NSNumber"},"numberWithUnsignedInt:",number_uint,true);install({"NSNumber"},"numberWithUnsignedInteger:",number_unsigned,true);install({"NSNumber"},"numberWithFloat:",number_float,true);install({"NSNumber"},"numberWithDouble:",number_double,true);
    install({"NSNumber"},"initWithBool:",number_init_bool);install({"NSNumber"},"initWithInt:",number_init_int);install({"NSNumber"},"initWithInteger:",number_init_integer);install({"NSNumber"},"initWithLongLong:",number_init_integer);install({"NSNumber"},"initWithUnsignedInt:",number_init_uint);install({"NSNumber"},"initWithUnsignedInteger:",number_init_unsigned);install({"NSNumber"},"initWithFloat:",number_init_float);install({"NSNumber"},"initWithDouble:",number_init_double);
    for(const char* s:{"intValue","integerValue","longValue","longLongValue"})install({"NSNumber"},s,number_integer_value);
    for(const char* s:{"unsignedIntValue","unsignedIntegerValue","unsignedLongValue","unsignedLongLongValue"})install({"NSNumber"},s,number_unsigned_value);
    install({"NSNumber"},"boolValue",number_bool_value);install({"NSNumber"},"floatValue",number_float_value);install({"NSNumber"},"doubleValue",number_double_value);install({"NSNumber"},"stringValue",number_string);

    auto arrays={"NSArray","NSMutableArray"};
    install(arrays,"array",collection_new,true);install(arrays,"arrayWithCapacity:",collection_class_capacity,true);install(arrays,"initWithCapacity:",collection_capacity);install(arrays,"arrayWithObject:",array_one,true);install(arrays,"arrayWithObjects:",foundation_array_objects,true);install(arrays,"initWithObjects:",foundation_array_objects);install(arrays,"arrayWithObjects:count:",array_counted,true);install(arrays,"initWithArray:",array_copy_init);install(arrays,"arrayWithArray:",array_copy_class,true);
    install(arrays,"count",array_count);install(arrays,"objectAtIndex:",array_at);install(arrays,"objectAtIndexedSubscript:",array_at);install(arrays,"firstObject",array_first);install(arrays,"lastObject",array_last);install(arrays,"addObject:",array_add);install(arrays,"addObjectsFromArray:",array_add_array);install(arrays,"containsObject:",array_contains);install(arrays,"indexOfObject:",array_index);install(arrays,"removeObject:",array_remove);install(arrays,"removeObjectIdenticalTo:",array_remove_identical);install(arrays,"removeObjectAtIndex:",array_remove_index);install(arrays,"removeAllObjects",array_clear);install(arrays,"removeLastObject",array_remove_last);install(arrays,"insertObject:atIndex:",array_insert);install(arrays,"replaceObjectAtIndex:withObject:",array_replace);install(arrays,"exchangeObjectAtIndex:withObjectAtIndex:",array_exchange);
    install(arrays,"componentsJoinedByString:",array_join);install(arrays,"objectEnumerator",array_enumerator);install(arrays,"reverseObjectEnumerator",array_reverse_enumerator);install(arrays,"countByEnumeratingWithState:objects:count:",array_enumerate);install(arrays,"makeObjectsPerformSelector:",array_perform);install(arrays,"makeObjectsPerformSelector:withObject:",array_perform_object);install({"NSEnumerator"},"nextObject",enumerator_next);
    install({"NSEnumerator"},"countByEnumeratingWithState:objects:count:",enumerator_enumerate);
    install(arrays,"copyWithZone:",array_copy_zone);install(arrays,"mutableCopyWithZone:",array_copy_zone);
    install({"NSMutableArray"},"sortUsingFunction:context:",array_sort_function);

    auto dicts={"NSDictionary","NSMutableDictionary"};
    install(dicts,"dictionary",collection_new,true);install(dicts,"dictionaryWithCapacity:",collection_class_capacity,true);install(dicts,"initWithCapacity:",collection_capacity);install(dicts,"dictionaryWithContentsOfFile:",plist_file,true);install(dicts,"initWithContentsOfFile:",dict_file_init);install(dicts,"dictionaryWithObject:forKey:",dict_one,true);install(dicts,"dictionaryWithObjects:forKeys:",dict_arrays,true);install(dicts,"dictionaryWithObjectsAndKeys:",foundation_dictionary_objects_keys,true);install(dicts,"initWithObjectsAndKeys:",foundation_dictionary_objects_keys);install(dicts,"dictionaryWithDictionary:",dict_copy_class,true);install(dicts,"initWithDictionary:",dict_copy_init);install(dicts,"addEntriesFromDictionary:",dict_add_dictionary);
    install(dicts,"count",dict_count);install(dicts,"objectForKey:",dict_get);install(dicts,"objectForKeyedSubscript:",dict_get);install(dicts,"valueForKey:",dict_get);install(dicts,"setObject:forKey:",dict_set);install(dicts,"setObject:forKeyedSubscript:",dict_set);install(dicts,"setValue:forKey:",dict_set);install(dicts,"removeObjectForKey:",dict_remove);install(dicts,"removeAllObjects",dict_clear);install(dicts,"allKeys",dict_keys);install(dicts,"allValues",dict_values);install(dicts,"keyEnumerator",dict_key_enumerator);install(dicts,"objectEnumerator",dict_value_enumerator);install(dicts,"countByEnumeratingWithState:objects:count:",dict_enumerate);
    install(dicts,"copyWithZone:",dict_copy_zone);install(dicts,"mutableCopyWithZone:",dict_copy_zone);
    auto sets={"NSSet","NSMutableSet"};install(sets,"set",collection_new,true);install(sets,"setWithObject:",set_one,true);install(sets,"initWithCapacity:",collection_capacity);install(sets,"count",array_count);install(sets,"addObject:",set_add);install(sets,"removeObject:",array_remove);install(sets,"containsObject:",array_contains);install(sets,"member:",set_member);install(sets,"removeAllObjects",array_clear);install(sets,"anyObject",array_first);install(sets,"allObjects",set_all_objects);install(sets,"objectEnumerator",array_enumerator);install(sets,"countByEnumeratingWithState:objects:count:",array_enumerate);install(sets,"copyWithZone:",set_copy_zone);install(sets,"mutableCopyWithZone:",set_copy_zone);

    install({"NSBundle"},"mainBundle",bundle_main,true);install({"NSBundle"},"bundlePath",bundle_path);install({"NSBundle"},"resourcePath",bundle_path);install({"NSBundle"},"infoDictionary",bundle_info);install({"NSBundle"},"objectForInfoDictionaryKey:",bundle_info_key);install({"NSBundle"},"pathForResource:ofType:",bundle_resource);install({"NSBundle"},"pathForResource:ofType:inDirectory:",bundle_resource_directory);install({"NSBundle"},"localizedStringForKey:value:table:",bundle_localized);
    install({"NSFileManager"},"defaultManager",file_manager,true);install({"NSFileManager"},"fileExistsAtPath:",file_exists);install({"NSFileManager"},"fileExistsAtPath:isDirectory:",file_exists_dir);install({"NSFileManager"},"createDirectoryAtPath:withIntermediateDirectories:attributes:error:",file_create_directory);install({"NSFileManager"},"contentsAtPath:",file_contents);install({"NSFileManager"},"contentsOfDirectoryAtPath:error:",file_directory_contents);install({"NSFileManager"},"currentDirectoryPath",file_current_directory);
    install({"NSUserDefaults"},"standardUserDefaults",defaults_standard,true);install({"NSUserDefaults"},"objectForKey:",defaults_object);install({"NSUserDefaults"},"boolForKey:",defaults_bool);install({"NSUserDefaults"},"integerForKey:",defaults_integer);install({"NSUserDefaults"},"doubleForKey:",defaults_double);install({"NSUserDefaults"},"setBool:forKey:",defaults_set_bool);install({"NSUserDefaults"},"setInteger:forKey:",defaults_set_integer);install({"NSUserDefaults"},"setDouble:forKey:",defaults_set_double);install({"NSUserDefaults"},"setObject:forKey:",dict_set);install({"NSUserDefaults"},"removeObjectForKey:",dict_remove);install({"NSUserDefaults"},"registerDefaults:",defaults_register);install({"NSUserDefaults"},"synchronize",defaults_sync);
    install({"NSUbiquitousKeyValueStore"},"defaultStore",ubiquitous_default,true);install({"NSUbiquitousKeyValueStore"},"objectForKey:",defaults_object);install({"NSUbiquitousKeyValueStore"},"boolForKey:",defaults_bool);install({"NSUbiquitousKeyValueStore"},"doubleForKey:",defaults_double);install({"NSUbiquitousKeyValueStore"},"setBool:forKey:",ubiquitous_set_bool);install({"NSUbiquitousKeyValueStore"},"setDouble:forKey:",ubiquitous_set_double);install({"NSUbiquitousKeyValueStore"},"setObject:forKey:",ubiquitous_set_object);install({"NSUbiquitousKeyValueStore"},"removeObjectForKey:",dict_remove);install({"NSUbiquitousKeyValueStore"},"dictionaryRepresentation",ubiquitous_dictionary);install({"NSUbiquitousKeyValueStore"},"synchronize",ubiquitous_sync);
    install({"NSNotificationCenter"},"defaultCenter",notification_center,true);install({"NSNotificationCenter"},"addObserver:selector:name:object:",notification_observe);install({"NSNotificationCenter"},"removeObserver:",notification_remove);install({"NSNotificationCenter"},"removeObserver:name:object:",notification_remove_named);install({"NSNotificationCenter"},"postNotificationName:object:",notification_post_simple);install({"NSNotificationCenter"},"postNotificationName:object:userInfo:",notification_post);install({"NSNotification"},"name",notification_name);install({"NSNotification"},"object",notification_object);install({"NSNotification"},"userInfo",notification_info);
    install({"NSThread"},"currentThread",current_thread,true);install({"NSThread"},"mainThread",current_thread,true);install({"NSThread"},"isMainThread",thread_main,true);install({"NSRunLoop"},"currentRunLoop",current_runloop,true);install({"NSRunLoop"},"mainRunLoop",current_runloop,true);install({"NSLock","NSRecursiveLock"},"lock",lock_lock);install({"NSLock","NSRecursiveLock"},"unlock",lock_unlock);install({"NSLock","NSRecursiveLock"},"tryLock",lock_try);
    auto data={"NSData","NSMutableData"};install(data,"data",data_new,true);install(data,"dataWithBytes:length:",data_bytes_class,true);install(data,"initWithBytes:length:",data_bytes_init);install(data,"dataWithLength:",data_length_class,true);install(data,"dataWithContentsOfFile:",read_data,true);install(data,"initWithContentsOfFile:",data_init_file);install(data,"dataWithContentsOfURL:",data_url,true);install(data,"initWithContentsOfURL:",data_init_url);install(data,"bytes",data_bytes);install(data,"mutableBytes",data_mutable_bytes);install(data,"length",data_length);install(data,"setLength:",data_set_length);install(data,"appendBytes:length:",data_append);install(data,"writeToFile:atomically:",file_write_data);
    install({"NSURL"},"URLWithString:",url_new,true);install({"NSURL"},"fileURLWithPath:",url_new,true);install({"NSURL"},"initWithString:",url_init);install({"NSURL"},"initFileURLWithPath:",url_init);install({"NSURL"},"path",url_path);install({"NSURL"},"absoluteString",url_path);install({"NSURL"},"isFileURL",url_file);
    install({"NSCharacterSet"},"characterSetWithCharactersInString:",character_set,true);install({"NSCharacterSet"},"whitespaceAndNewlineCharacterSet",character_set_whitespace_newline,true);install({"NSCharacterSet"},"whitespaceCharacterSet",character_set_whitespace,true);install({"NSCharacterSet"},"newlineCharacterSet",character_set_newline,true);install({"NSCharacterSet"},"characterIsMember:",character_set_member);
    install({"NSXMLParser"},"initWithData:",xml_init_data);install({"NSXMLParser"},"initWithContentsOfURL:",xml_init_url);install({"NSXMLParser"},"setDelegate:",set_delegate);install({"NSXMLParser"},"delegate",get_delegate);install({"NSXMLParser"},"parse",xml_parse);install({"NSXMLParser"},"setShouldProcessNamespaces:",xml_config);install({"NSXMLParser"},"setShouldReportNamespacePrefixes:",xml_config);install({"NSXMLParser"},"setShouldResolveExternalEntities:",[](Obj,Sel,bool enable){if(enable)rt_fail("Foundation XML: external entities unsupported");});
    install({"NSProcessInfo"},"processInfo",process_info,true);install({"NSProcessInfo"},"processorCount",processor_count);install({"NSProcessInfo"},"activeProcessorCount",processor_count);install({"NSLocale"},"currentLocale",locale_current,true);install({"NSLocale"},"localeIdentifier",locale_identifier);install({"NSLocale"},"preferredLanguages",locale_languages,true);
    install({"NSError"},"errorWithDomain:code:userInfo:",error_new,true);install({"NSError"},"domain",error_domain);install({"NSError"},"code",error_code);install({"NSError"},"userInfo",error_info);install({"NSError"},"localizedDescription",error_description);
    // Version is the deliberately exposed compatibility contract, not the
    // identity/version of an Apple device. Keep aligned with core CF version.
    install({"UIDevice"},"currentDevice",device_current,true);install({"UIDevice"},"systemVersion",device_system_version);install({"UIDevice"},"systemName",device_system_name);install({"UIDevice"},"name",device_name);install({"UIDevice"},"model",device_model);install({"UIDevice"},"localizedModel",device_model);install({"UIDevice"},"userInterfaceIdiom",device_idiom);install({"UIDevice"},"orientation",device_orientation);install({"UIDevice"},"beginGeneratingDeviceOrientationNotifications",device_begin_orientation);install({"UIDevice"},"endGeneratingDeviceOrientationNotifications",device_end_orientation);install({"UIDevice"},"isGeneratingDeviceOrientationNotifications",device_orientation_active);
}

// Called by the core immediately before it erases HostData and frees an
// allocated host object. Move ownership out before releasing children.
void rt_foundation_dealloc(Obj object){
    std::string cls=rt_class_name(object);
    if(auto i=invocations.find(object);i!=invocations.end()){Obj signature=i->second.signature;invocations.erase(i);drop(signature);}
    if(cls=="NSArray"||cls=="NSMutableArray"||cls=="NSSet"||cls=="NSMutableSet"||cls=="NSEnumerator")array_clear(object,nullptr);
    if(cls=="NSDictionary"||cls=="NSMutableDictionary"||cls=="NSUserDefaults"||cls=="NSUbiquitousKeyValueStore")dict_clear(object,nullptr);
    observers.erase(object);locks.erase(object);enumeration_position.erase(object);mutation_words.erase(object);
}

extern "C" Obj foundation_NSClassFromString(Obj value){return rt_class(rt_utf8(value));}
extern "C" Obj foundation_NSStringFromSelector(Sel value){return rt_string(value?value:"");}
extern "C" Obj foundation_NSSearchPathForDirectoriesInDomains(uint64_t directory,uint64_t,bool){
    const char* suffix=directory==9?"Documents":directory==13?"Caches":directory==5?"Library":nullptr;
    if(!suffix)rt_fail("Foundation: unsupported NSSearchPath directory");
    fs::path path=fs::path(rt_data_root)/".ipa-study"/suffix;std::error_code ec;fs::create_directories(path,ec);if(ec)rt_fail("Foundation: cannot create application data directory");return array({ns(path.string())});
}
extern "C" void foundation_CFRelease(Obj value){if(value)send<void>(value,"release");}
extern "C" const char* foundation_CFStringGetCStringPtr(Obj value,uint32_t encoding){return encoding==0x08000100||encoding==0?rt_utf8(value):nullptr;}
extern "C" int64_t foundation_CFStringCompare(Obj first,Obj second,uint64_t flags){return string_compare_options(first,nullptr,second,flags);}
extern "C" Obj foundation_CFURLCopyPathExtension(Obj value){return string_path_extension(url_path(value,nullptr),nullptr);}
void* rt_foundation_symbol(const char* name){
    struct Entry{const char* name;void* value;};
    static Entry symbols[]={
        {"NSClassFromString",reinterpret_cast<void*>(foundation_NSClassFromString)},
        {"NSStringFromSelector",reinterpret_cast<void*>(foundation_NSStringFromSelector)},
        {"NSSearchPathForDirectoriesInDomains",reinterpret_cast<void*>(foundation_NSSearchPathForDirectoriesInDomains)},
        {"CFRelease",reinterpret_cast<void*>(foundation_CFRelease)},
        {"CFStringGetCStringPtr",reinterpret_cast<void*>(foundation_CFStringGetCStringPtr)},
        {"CFStringCompare",reinterpret_cast<void*>(foundation_CFStringCompare)},
        {"CFURLCopyPathExtension",reinterpret_cast<void*>(foundation_CFURLCopyPathExtension)},
    };
    if(name[0]=='_')++name;
    for(auto& symbol:symbols)if(std::strcmp(name,symbol.name)==0)return symbol.value;
    return nullptr;
}
