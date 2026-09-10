#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// The callback sees the original Mach-O symbol, including its leading underscore.
// weak means BIND_SYMBOL_FLAGS_WEAK_IMPORT, not merely the weak-coalescing stream.
using MachResolver = void* (*)(const char* name, bool weak, void* user);

struct MachSection {
    std::string segment, name;
    void* address = nullptr;
    size_t size = 0;
    uint32_t flags = 0;
};

struct MachSegment {
    std::string name;
    uint64_t vmaddr = 0, vmsize = 0;
    void* address = nullptr; // __PAGEZERO stays nullptr and is never mapped.
    int protection = 0;     // Native PROT_READ / PROT_WRITE / PROT_EXEC bits.
};

struct MachImage {
    void* mapping = nullptr;
    size_t mapping_size = 0;
    void* base = nullptr;   // Loaded Mach-O header, normally start of __TEXT.
    void* entry = nullptr;  // LC_MAIN only; loader never invokes it.
    uintptr_t slide = 0;
    uint64_t preferred_base = 0;
    std::vector<MachSegment> segments;
    std::vector<MachSection> sections;
    std::vector<void*> mod_init_functions; // Already rebased, never called here.
    std::unordered_map<std::string, void*> symbols;
    size_t rebase_count = 0, bind_count = 0, weak_bind_count = 0, lazy_bind_count = 0;

    MachSection* find_section(const char* segment, const char* name);
    const MachSection* find_section(const char* segment, const char* name) const;
    void* address_for_vmaddr(uint64_t address, size_t size = 1) const;
    void* find_symbol(const char* name) const;
};

// Restricted prototype: little-endian ARM64 MH_EXECUTE + LC_MAIN + classic dyld
// info. Loads/fixes/protects only; it does not provide a Darwin ABI or run +load.
// All dependencies use one caller-controlled symbol namespace. Defined symbols
// of this executable win locally, including during weak coalescing. This is not
// multi-image/two-level dyld resolution. Strong unresolved symbols are fatal.
// image must be empty; explicit macho_unload() owns the mapping lifecycle.
bool macho_load(const char* path, MachResolver resolver, void* user,
                MachImage& image, std::string& error);
void macho_unload(MachImage& image);
