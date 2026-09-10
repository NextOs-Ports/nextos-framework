// Independent behavioral check against the host's explicitly selected C
// locale. Also read via raw ABI offsets, as inlined Darwin guest code would.
#include "../prototype/darwin_libc.cpp"
#include <cassert>
#include <cctype>
#include <clocale>
#include <iostream>
#include <stdexcept>

[[noreturn]]void rt_fail(const char* reason){throw std::runtime_error(reason);}
int main(){
    assert(std::setlocale(LC_CTYPE,"C"));
    auto base=static_cast<const unsigned char*>(rt_darwin_symbol("__DefaultRuneLocale"));
    auto types=reinterpret_cast<const uint32_t*>(base+60);
    auto lower=reinterpret_cast<const int32_t*>(base+1084);
    auto upper=reinterpret_cast<const int32_t*>(base+2108);
    assert(std::memcmp(base,"RuneMagA",8)==0);
    assert(std::strcmp(reinterpret_cast<const char*>(base+8),"NONE")==0);
    for(unsigned c=0;c<256;++c){
        assert(bool(types[c]&0x100)==bool(std::isalpha(c)));
        assert(bool(types[c]&0x200)==bool(std::iscntrl(c)));
        assert(bool(types[c]&0x400)==bool(std::isdigit(c)));
        assert(bool(types[c]&0x800)==bool(std::isgraph(c)));
        assert(bool(types[c]&0x1000)==bool(std::islower(c)));
        assert(bool(types[c]&0x2000)==bool(std::ispunct(c)));
        assert(bool(types[c]&0x4000)==bool(std::isspace(c)));
        assert(bool(types[c]&0x8000)==bool(std::isupper(c)));
        assert(bool(types[c]&0x10000)==bool(std::isxdigit(c)));
        assert(bool(types[c]&0x20000)==bool(std::isblank(c)));
        assert(bool(types[c]&0x40000)==bool(std::isprint(c)));
        assert(lower[c]==std::tolower(c));assert(upper[c]==std::toupper(c));
        unsigned expected=0;if(c>='0'&&c<='9')expected=c-'0';else if(c>='A'&&c<='F')expected=c-'A'+10;else if(c>='a'&&c<='f')expected=c-'a'+10;
        assert((types[c]&0xff)==expected);
    }
    auto mask=reinterpret_cast<int32_t(*)(int32_t,uint64_t)>(rt_darwin_symbol("___maskrune"));
    assert(mask('A',0x100)==0x100);assert(mask(-1,~uint64_t(0))==0);
    for(int rune:{-2,256,0x1f600}){bool failed=false;try{mask(rune,~uint64_t(0));}catch(const std::runtime_error&){failed=true;}assert(failed);}
    assert(*static_cast<const void* const*>(rt_darwin_symbol("__CurrentRuneLocale"))==base);
    assert(rt_darwin_symbol("_unimplemented_unicode_api")==nullptr);
    std::cout<<"{\"status\":\"PASS\",\"locale\":\"C/NONE\",\"bytes_compared\":256,\"classifications_per_byte\":11,\"case_maps_per_byte\":2,\"hex_values\":256,\"abi_size\":3208,\"types_offset\":60,\"lower_offset\":1084,\"upper_offset\":2108,\"outside_contract_rejected\":3,\"executes_guest_code\":false}\n";
}
