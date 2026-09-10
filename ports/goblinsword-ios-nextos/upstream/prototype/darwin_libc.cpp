#include "runtime.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

// Interoperability layout reconstructed from Apple's public LP64 ABI:
// https://github.com/apple-oss-distributions/Libc/blob/main/include/runetype.h
// Classification values/default C locale verified against:
// https://github.com/apple-oss-distributions/Libc/blob/main/include/_ctype.h
// https://github.com/apple-oss-distributions/Libc/blob/main/locale/FreeBSD/table.c
// This implementation generates the table from ASCII character categories;
// it does not embed Apple's source table or provide a Unicode locale.
namespace {
struct DarwinRuneEntry { int32_t first, last, mapping; const uint32_t* types; };
struct DarwinRuneRange { int32_t count; const DarwinRuneEntry* ranges; };
struct DarwinRuneCharClass { char name[14]; uint32_t mask; };
struct DarwinRuneLocale {
    char magic[8];
    char encoding[32];
    int32_t (*read_rune)(const char*, uint64_t, const char**);
    int32_t (*write_rune)(int32_t, char*, uint64_t, char**);
    int32_t invalid_rune;
    uint32_t types[256];
    int32_t lower[256];
    int32_t upper[256];
    DarwinRuneRange extended_types, extended_lower, extended_upper;
    const void* variable;
    int32_t variable_size, class_count;
    const DarwinRuneCharClass* classes;
};
static_assert(sizeof(void*) == 8, "Darwin rune bridge requires LP64");
static_assert(offsetof(DarwinRuneLocale, types) == 60);
static_assert(offsetof(DarwinRuneLocale, lower) == 1084);
static_assert(offsetof(DarwinRuneLocale, upper) == 2108);
static_assert(offsetof(DarwinRuneLocale, extended_types) == 3136);
static_assert(offsetof(DarwinRuneLocale, classes) == 3200);
static_assert(sizeof(DarwinRuneLocale) == 3208);

constexpr uint32_t alpha=0x100, control=0x200, digit=0x400, graph=0x800,
    lowercase=0x1000, punctuation=0x2000, space=0x4000, uppercase=0x8000,
    hexadecimal=0x10000, blank=0x20000, printable=0x40000;

constexpr DarwinRuneLocale build_default_locale() {
    DarwinRuneLocale result{};
    const char magic[] = "RuneMagA";
    const char encoding[] = "NONE";
    for (unsigned i=0;i<8;++i) result.magic[i]=magic[i];
    for (unsigned i=0;i<sizeof encoding;++i) result.encoding[i]=encoding[i];
    result.invalid_rune=0xfffd;
    // The source locale's deprecated conversion callbacks are null; its
    // extended ranges are empty. Bytes 128..255 have identity case mapping.
    for (unsigned c=0;c<256;++c) {
        result.lower[c]=c;
        result.upper[c]=c;
        if(c>=128) continue;
        uint32_t bits=0;
        bool is_upper=c>='A'&&c<='Z', is_lower=c>='a'&&c<='z';
        bool is_digit=c>='0'&&c<='9';
        if(c<32||c==127)bits|=control;
        if(c==' '||(c>='\t'&&c<='\r'))bits|=space;
        if(c==' '||c=='\t')bits|=blank;
        if(c>=32&&c<=126)bits|=printable;
        if(c>=33&&c<=126)bits|=graph;
        if(is_upper){bits|=alpha|uppercase;result.lower[c]=c+('a'-'A');}
        if(is_lower){bits|=alpha|lowercase;result.upper[c]=c-('a'-'A');}
        if(is_digit)bits|=digit;
        if(c>=33&&c<=126&&!is_upper&&!is_lower&&!is_digit)bits|=punctuation;
        if(is_digit)bits|=hexadecimal|(c-'0');
        if(c>='a'&&c<='f')bits|=hexadecimal|(c-'a'+10);
        if(c>='A'&&c<='F')bits|=hexadecimal|(c-'A'+10);
        result.types[c]=bits;
    }
    return result;
}
constexpr DarwinRuneLocale default_locale=build_default_locale();
const DarwinRuneLocale* current_locale=&default_locale;

int checked_byte(int32_t rune) {
    if(rune==-1) return -1; // EOF
    if(rune<0||rune>=256)rt_fail("Darwin libc: rune outside byte/C-locale contract; Unicode locale is not implemented");
    return rune;
}
int32_t mask_rune(int32_t rune,uint64_t mask){int c=checked_byte(rune);return c<0?0:static_cast<int32_t>(default_locale.types[c]&static_cast<uint32_t>(mask));}
uint64_t rune_type(int32_t rune){int c=checked_byte(rune);return c<0?0:default_locale.types[c];}
int32_t lower_rune(int32_t rune){int c=checked_byte(rune);return c<0?c:default_locale.lower[c];}
int32_t upper_rune(int32_t rune){int c=checked_byte(rune);return c<0?c:default_locale.upper[c];}
}

// Receives original Mach-O names, INCLUDING the object-format underscore.
// The locale symbol resolves to DATA, never to a callable missing-symbol stub.
void* rt_darwin_symbol(const char* name) {
    if(std::strcmp(name,"__DefaultRuneLocale")==0)return const_cast<DarwinRuneLocale*>(&default_locale);
    if(std::strcmp(name,"__CurrentRuneLocale")==0)return &current_locale;
    if(std::strcmp(name,"___maskrune")==0)return reinterpret_cast<void*>(mask_rune);
    if(std::strcmp(name,"____runetype")==0)return reinterpret_cast<void*>(rune_type);
    if(std::strcmp(name,"___tolower")==0||std::strcmp(name,"____tolower")==0)return reinterpret_cast<void*>(lower_rune);
    if(std::strcmp(name,"___toupper")==0||std::strcmp(name,"____toupper")==0)return reinterpret_cast<void*>(upper_rune);
    return nullptr;
}
