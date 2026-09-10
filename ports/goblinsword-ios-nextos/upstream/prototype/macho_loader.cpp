#include "macho_loader.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>

// Format references (implementation below is original): Apple mach-o/loader.h
// and dyld classic rebase/bind bytecode. No Apple runtime is linked or copied.
namespace {
constexpr uint32_t LC_SEGMENT_64 = 0x19, LC_DYLD_INFO = 0x22;
constexpr uint32_t LC_DYLD_INFO_ONLY = 0x80000022, LC_MAIN = 0x80000028;
constexpr uint32_t LC_SYMTAB = 2, LC_ENCRYPTION_INFO = 0x21, LC_ENCRYPTION_INFO_64 = 0x2c;
constexpr size_t MAX_FILE_SIZE = 128 * 1024 * 1024;
constexpr uint64_t MAX_MAPPING_SIZE = 512ull * 1024 * 1024;
constexpr size_t MAX_FIXUPS = 16 * 1024 * 1024;

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }
void require(bool value, const std::string& message) { if (!value) fail(message); }
bool range(uint64_t offset, uint64_t size, uint64_t total) {
    return offset <= total && size <= total - offset;
}
std::string fixed_name(const uint8_t* p) {
    return std::string(reinterpret_cast<const char*>(p), strnlen(reinterpret_cast<const char*>(p), 16));
}
uint32_t u32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint64_t u64(const uint8_t* p) { return uint64_t(u32(p)) | uint64_t(u32(p + 4)) << 32; }
std::string number(uint64_t v) { return std::to_string(v); }

struct Stream { uint32_t offset = 0, size = 0; };
struct RawSegment { uint64_t fileoff = 0, filesize = 0; };
struct Reader {
    const uint8_t *start, *current, *end;
    std::string label;
    Reader(const std::vector<uint8_t>& bytes, Stream s, const char* name)
        : start(bytes.data() + s.offset), current(start), end(start + s.size), label(name) {}
    bool empty() const { return current == end; }
    [[noreturn]] void bad(const std::string& message) const {
        fail(label + " +" + number(current - start) + ": " + message);
    }
    uint8_t byte() { if (empty()) bad("truncated bytecode"); return *current++; }
    uint64_t uleb() {
        uint64_t result = 0;
        for (unsigned i = 0; i != 10; ++i) {
            uint8_t b = byte();
            if (i == 9 && (b & 0xfe)) bad("ULEB128 overflow");
            result |= uint64_t(b & 0x7f) << (7 * i);
            if (!(b & 0x80)) return result;
        }
        bad("ULEB128 overflow");
    }
    int64_t sleb() {
        uint64_t result = 0;
        for (unsigned i = 0; i != 10; ++i) {
            uint8_t b = byte();
            if (i == 9 && b != 0 && b != 0x7f) bad("SLEB128 overflow");
            result |= uint64_t(b & 0x7f) << (7 * i);
            if (!(b & 0x80)) {
                unsigned bits = 7 * (i + 1);
                if (bits < 64 && (b & 0x40)) result |= ~uint64_t(0) << bits;
                return static_cast<int64_t>(result);
            }
        }
        bad("SLEB128 overflow");
    }
    std::string string() {
        const uint8_t* first = current;
        while (current != end && *current) ++current;
        if (current == end) bad("unterminated symbol");
        std::string out(reinterpret_cast<const char*>(first), current - first);
        ++current;
        return out;
    }
    void padding_only() {
        while (!empty()) if (byte() != 0) bad("nonzero byte after DONE");
    }
};

struct Fixups {
    MachImage& image;
    MachResolver resolver;
    void* user;
    size_t budget = MAX_FIXUPS;

    uint8_t* location(Reader& r, size_t index, uint64_t offset, size_t width) {
        if (index >= image.segments.size()) r.bad("segment index out of range");
        auto& segment = image.segments[index];
        if (!segment.address || !range(offset, width, segment.vmsize))
            r.bad("fixup outside segment " + segment.name + " offset=" + number(offset));
        if (!budget--) r.bad("fixup budget exhausted");
        return static_cast<uint8_t*>(segment.address) + offset;
    }
    void advance(Reader& r, size_t index, uint64_t& offset, uint64_t delta) {
        if (index >= image.segments.size() || !image.segments[index].address)
            r.bad("address advance without mapped segment");
        // Classic dyld uses unsigned arithmetic. In this image the weak stream
        // explicitly encodes UINT64_MAX-7 as skip, cancelling the pointer step.
        offset += delta;
        if (offset > image.segments[index].vmsize) r.bad("address advance leaves segment");
    }
    void rebase(const std::vector<uint8_t>& bytes, Stream stream) {
        if (!stream.size) return;
        Reader r(bytes, stream, "rebase");
        size_t segment = size_t(-1);
        uint64_t offset = 0;
        unsigned type = 0;
        auto one = [&]() {
            if (type != 1) r.bad("only ARM64 pointer rebases are supported; type=" + number(type));
            uint8_t* p = location(r, segment, offset, 8);
            uint64_t value = u64(p);
            // A classic pointer rebase must refer to this mapped image.
            if (!image.address_for_vmaddr(value, 0)) r.bad("rebase target outside image: " + number(value));
            value += image.slide;
            std::memcpy(p, &value, 8);
            ++image.rebase_count;
        };
        auto many = [&](uint64_t count, uint64_t step) {
            if (count > budget) r.bad("rebase count exceeds budget");
            for (uint64_t i = 0; i < count; ++i) { one(); advance(r, segment, offset, step); }
        };
        while (!r.empty()) {
            uint8_t b = r.byte(), op = b & 0xf0, imm = b & 0x0f;
            switch (op) {
            case 0x00: r.padding_only(); return;
            case 0x10: type = imm; break;
            case 0x20: segment = imm; offset = r.uleb(); advance(r, segment, offset, 0); break;
            case 0x30: advance(r, segment, offset, r.uleb()); break;
            case 0x40: advance(r, segment, offset, uint64_t(imm) * 8); break;
            case 0x50: many(imm, 8); break;
            case 0x60: many(r.uleb(), 8); break;
            case 0x70: { uint64_t skip = r.uleb(); one(); advance(r, segment, offset, skip + 8); break; }
            case 0x80: { uint64_t count = r.uleb(), skip = r.uleb(); many(count, skip + 8); break; }
            default: r.bad("unsupported opcode " + number(op));
            }
        }
        r.bad("missing DONE");
    }
    void bind(const std::vector<uint8_t>& bytes, Stream stream, bool lazy, bool coalescing) {
        if (!stream.size) return;
        Reader r(bytes, stream, lazy ? "lazy_bind" : coalescing ? "weak_bind" : "bind");
        size_t segment = size_t(-1);
        uint64_t offset = 0;
        unsigned type = 1, flags = 0;
        int64_t addend = 0, ordinal = 0;
        std::string symbol;
        bool resolved = false;
        uintptr_t target = 0;
        auto one = [&]() {
            if (type != 1) r.bad("only ARM64 pointer bindings are supported; type=" + number(type));
            if (symbol.empty()) r.bad("binding without symbol");
            uint8_t* p = location(r, segment, offset, 8);
            bool weak = (flags & 1) != 0;
            if (!resolved) {
                void* value = image.find_symbol(symbol.c_str());
                if (!value && resolver) value = resolver(symbol.c_str(), weak, user);
                if (!value && !weak)
                    r.bad("unresolved strong symbol " + symbol + " ordinal=" + std::to_string(ordinal));
                target = reinterpret_cast<uintptr_t>(value);
                resolved = true;
            }
            uint64_t value = target ? target + uint64_t(addend) : 0;
            std::memcpy(p, &value, 8);
            if (lazy) ++image.lazy_bind_count;
            else if (coalescing) ++image.weak_bind_count;
            else ++image.bind_count;
        };
        bool ended = false;
        while (!r.empty()) {
            uint8_t b = r.byte(), op = b & 0xf0, imm = b & 0x0f;
            ended = false;
            switch (op) {
            case 0x00:
                if (!lazy) { r.padding_only(); return; }
                segment = size_t(-1); offset = 0; type = 1; flags = 0;
                addend = 0; ordinal = 0; symbol.clear(); resolved = false;
                ended = true;
                break;
            case 0x10: ordinal = imm; resolved = false; break;
            case 0x20: {
                uint64_t v = r.uleb();
                if (v > uint64_t(INT64_MAX)) r.bad("library ordinal overflow");
                ordinal = int64_t(v); resolved = false; break;
            }
            case 0x30: ordinal = imm ? int64_t(int8_t(0xf0 | imm)) : 0; resolved = false; break;
            case 0x40:
                flags = imm; symbol = r.string(); resolved = false;
                if (flags & ~unsigned(9)) r.bad("unknown symbol flags");
                break;
            case 0x50: type = imm; break;
            case 0x60: addend = r.sleb(); break;
            case 0x70: segment = imm; offset = r.uleb(); advance(r, segment, offset, 0); break;
            case 0x80: advance(r, segment, offset, r.uleb()); break;
            case 0x90: one(); advance(r, segment, offset, 8); break;
            case 0xa0: { uint64_t skip = r.uleb(); one(); advance(r, segment, offset, skip + 8); break; }
            case 0xb0: one(); advance(r, segment, offset, uint64_t(imm + 1) * 8); break;
            case 0xc0: {
                uint64_t count = r.uleb(), skip = r.uleb();
                if (count > budget) r.bad("bind count exceeds budget");
                for (uint64_t i = 0; i < count; ++i) { one(); advance(r, segment, offset, skip + 8); }
                break;
            }
            default: r.bad("unsupported opcode (including threaded bind): " + number(op));
            }
        }
        if (!ended) r.bad("missing DONE");
    }
};
} // namespace

MachSection* MachImage::find_section(const char* segment, const char* name) {
    for (auto& s : sections) if (s.segment == segment && s.name == name) return &s;
    return nullptr;
}
const MachSection* MachImage::find_section(const char* segment, const char* name) const {
    for (const auto& s : sections) if (s.segment == segment && s.name == name) return &s;
    return nullptr;
}
void* MachImage::address_for_vmaddr(uint64_t address, size_t size) const {
    for (const auto& s : segments)
        if (s.address && address >= s.vmaddr && range(address - s.vmaddr, size, s.vmsize))
            return static_cast<uint8_t*>(s.address) + (address - s.vmaddr);
    return nullptr;
}
void* MachImage::find_symbol(const char* name) const {
    auto it = symbols.find(name);
    return it == symbols.end() ? nullptr : it->second;
}
void macho_unload(MachImage& image) {
    if (image.mapping) munmap(image.mapping, image.mapping_size);
    image = MachImage{};
}

bool macho_load(const char* path, MachResolver resolver, void* user,
                MachImage& image, std::string& error) {
    error.clear();
    if (image.mapping) { error = "output image already owns a mapping"; return false; }
    MachImage loaded;
    try {
        static_assert(sizeof(void*) == 8, "Mach-O ARM64 loader requires 64-bit host");
        uint16_t endian = 1;
        require(*reinterpret_cast<uint8_t*>(&endian) == 1, "big-endian host unsupported");
        require(path != nullptr, "null image path");
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        require(file.good(), "cannot open Mach-O image");
        auto end = file.tellg();
        require(end >= 32 && uint64_t(end) <= MAX_FILE_SIZE, "invalid or oversized image file");
        std::vector<uint8_t> bytes(static_cast<size_t>(end));
        file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        require(file.good(), "short read of Mach-O image");
        const uint8_t* h = bytes.data();
        require(u32(h) == 0xfeedfacf && u32(h + 4) == 0x0100000c, "expected little-endian ARM64 Mach-O");
        require((u32(h + 8) & 0xffffff) == 0, "ARM64 subtype unsupported (including arm64e/PAC)");
        require(u32(h + 12) == 2, "only MH_EXECUTE is supported");
        require((u32(h + 24) & 0x200000) != 0, "image must support a slide (MH_PIE)");
        uint32_t ncmds = u32(h + 16), command_bytes = u32(h + 20);
        require(ncmds <= 4096 && range(32, command_bytes, bytes.size()), "invalid load command bounds");
        size_t position = 32, command_end = 32 + command_bytes;
        std::vector<RawSegment> raw;
        Stream rebase, bind, weak, lazy, exports;
        uint32_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
        bool have_dyld = false, have_main = false, have_symtab = false;
        uint64_t entryoff = 0, minimum = UINT64_MAX, maximum = 0;
        long page_long = sysconf(_SC_PAGESIZE);
        require(page_long > 0, "cannot determine host page size");
        uint64_t page = uint64_t(page_long);
        require((page & (page - 1)) == 0, "invalid host page size");
        for (uint32_t i = 0; i < ncmds; ++i) {
            require(range(position, 8, command_end), "truncated load command header");
            const uint8_t* c = h + position;
            uint32_t cmd = u32(c), size = u32(c + 4);
            require(size >= 8 && !(size & 7) && range(position, size, command_end), "invalid load command size");
            if (cmd == LC_SEGMENT_64) {
                require(size >= 72, "short LC_SEGMENT_64");
                MachSegment s;
                s.name = fixed_name(c + 8); s.vmaddr = u64(c + 24); s.vmsize = u64(c + 32);
                RawSegment r{u64(c + 40), u64(c + 48)};
                uint32_t maximum_protection = u32(c + 56), protection = u32(c + 60);
                uint32_t section_count = u32(c + 64);
                require(section_count <= 4096 && uint64_t(section_count) * 80 + 72 == size, "invalid section count");
                require(!(protection & ~7u) && !(maximum_protection & ~7u) && !(protection & ~maximum_protection), "invalid segment protection");
                require((protection & 6) != 6, "writable executable segment rejected");
                s.protection = int(protection); // Darwin VM_PROT bits equal Linux PROT bits.
                require(range(r.fileoff, r.filesize, bytes.size()) && r.filesize <= s.vmsize, "segment file range invalid");
                require(s.vmsize <= UINT64_MAX - s.vmaddr, "segment VM range overflows");
                if (s.name == "__PAGEZERO") {
                    require(s.vmaddr == 0 && !r.filesize && !protection && !maximum_protection && !section_count, "invalid PAGEZERO");
                } else {
                    require(s.vmsize && !(s.vmaddr & (page - 1)) && !(s.vmsize & (page - 1)), "segment is not host-page aligned");
                    require(protection & 1, "non-readable segment unsupported");
                    minimum = std::min(minimum, s.vmaddr); maximum = std::max(maximum, s.vmaddr + s.vmsize);
                    for (const auto& prior : loaded.segments)
                        require(prior.name == "__PAGEZERO" || s.vmaddr >= prior.vmaddr + prior.vmsize || prior.vmaddr >= s.vmaddr + s.vmsize,
                                "overlapping VM segments");
                }
                loaded.segments.push_back(s); raw.push_back(r);
                for (uint32_t j = 0; j < section_count; ++j) {
                    const uint8_t* sc = c + 72 + uint64_t(j) * 80;
                    MachSection section;
                    section.name = fixed_name(sc); section.segment = fixed_name(sc + 16);
                    uint64_t addr = u64(sc + 32), count = u64(sc + 40);
                    uint32_t fileoff = u32(sc + 48), alignment = u32(sc + 52);
                    section.flags = u32(sc + 64);
                    require(section.segment == s.name && addr >= s.vmaddr && range(addr - s.vmaddr, count, s.vmsize), "section outside owning segment");
                    require(alignment < 32 && (addr & ((uint64_t(1) << alignment) - 1)) == 0, "invalid section alignment");
                    unsigned type = section.flags & 0xff;
                    require(type < 0x11 || type > 0x15, "Mach-O thread-local storage unsupported");
                    bool zero_fill = type == 1 || type == 0xc;
                    if (!zero_fill && count) {
                        require(fileoff >= r.fileoff && range(fileoff - r.fileoff, count, r.filesize), "section outside segment file bytes");
                        require(uint64_t(fileoff) - r.fileoff == addr - s.vmaddr, "section VM/file layout mismatch");
                    }
                    require(u32(sc + 60) == 0, "section relocations unsupported; classic dyld info required");
                    section.address = reinterpret_cast<void*>(addr); // Resolved after mmap.
                    section.size = size_t(count); loaded.sections.push_back(std::move(section));
                }
            } else if (cmd == LC_DYLD_INFO || cmd == LC_DYLD_INFO_ONLY) {
                require(size == 48 && !have_dyld, "invalid or duplicate dyld info"); have_dyld = true;
                Stream* streams[] = {&rebase, &bind, &weak, &lazy, &exports};
                for (unsigned j = 0; j < 5; ++j) {
                    *streams[j] = Stream{u32(c + 8 + j * 8), u32(c + 12 + j * 8)};
                    require(range(streams[j]->offset, streams[j]->size, bytes.size()), "dyld stream outside file");
                }
            } else if (cmd == LC_MAIN) {
                require(size == 24 && !have_main, "invalid or duplicate LC_MAIN"); have_main = true; entryoff = u64(c + 8);
            } else if (cmd == LC_SYMTAB) {
                require(size == 24 && !have_symtab, "invalid or duplicate LC_SYMTAB"); have_symtab = true;
                symoff = u32(c + 8); nsyms = u32(c + 12); stroff = u32(c + 16); strsize = u32(c + 20);
                require(range(symoff, uint64_t(nsyms) * 16, bytes.size()) && range(stroff, strsize, bytes.size()), "symbol table outside file");
            } else if (cmd == LC_ENCRYPTION_INFO || cmd == LC_ENCRYPTION_INFO_64) {
                require(size >= 20 && u32(c + 16) == 0, "encrypted executable unsupported");
            } else if (cmd == 0x80000034 || cmd == 0x80000033 || cmd == 5 || cmd == 4) {
                fail("chained fixups, separate exports trie, and thread entry commands unsupported");
            } else if (cmd & 0x80000000u) {
                // Weak-load/reexport/upward-load dylib commands name dependencies
                // whose symbols are supplied by the explicit caller namespace.
                require(cmd == 0x80000018 || cmd == 0x8000001f || cmd == 0x80000023 || cmd == 0x8000001c,
                        "unknown mandatory load command: " + number(cmd));
            }
            position += size;
        }
        require(position == command_end && have_dyld && have_main, "missing dyld info/LC_MAIN or command-size mismatch");
        require(minimum != UINT64_MAX && maximum > minimum && maximum - minimum <= MAX_MAPPING_SIZE, "invalid/oversized mapped span");
        loaded.mapping_size = size_t(maximum - minimum); loaded.preferred_base = minimum;
        loaded.mapping = mmap(nullptr, loaded.mapping_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (loaded.mapping == MAP_FAILED) { loaded.mapping = nullptr; fail("mmap: " + std::string(std::strerror(errno))); }
        loaded.slide = reinterpret_cast<uintptr_t>(loaded.mapping) - minimum;
        for (size_t i = 0; i < loaded.segments.size(); ++i) {
            auto& s = loaded.segments[i]; const auto& r = raw[i];
            if (s.name == "__PAGEZERO") continue;
            s.address = static_cast<uint8_t*>(loaded.mapping) + (s.vmaddr - minimum);
            require(mprotect(s.address, size_t(s.vmsize), PROT_READ | PROT_WRITE) == 0,
                    "mprotect segment for fixups: " + std::string(std::strerror(errno)));
            if (r.filesize) std::memcpy(s.address, h + r.fileoff, size_t(r.filesize));
            if (r.fileoff == 0 && r.filesize >= command_end) {
                require(!loaded.base, "ambiguous Mach-O header mapping"); loaded.base = s.address;
            }
            if ((s.protection & PROT_EXEC) && entryoff >= r.fileoff && range(entryoff - r.fileoff, 4, r.filesize)) {
                require(!loaded.entry, "ambiguous entry mapping");
                loaded.entry = static_cast<uint8_t*>(s.address) + (entryoff - r.fileoff);
            }
        }
        require(loaded.base && loaded.entry && !(reinterpret_cast<uintptr_t>(loaded.entry) & 3), "header or ARM64 entry not in appropriate segment");
        for (auto& section : loaded.sections)
            section.address = loaded.address_for_vmaddr(reinterpret_cast<uintptr_t>(section.address), section.size);
        for (uint32_t i = 0; i < nsyms; ++i) {
            const uint8_t* n = h + symoff + uint64_t(i) * 16;
            uint32_t strx = u32(n); uint8_t type = n[4]; uint64_t value = u64(n + 8);
            if (type & 0xe0) continue; // Debug/STAB entries are not symbols.
            require(strx < strsize, "symbol string index outside table");
            const char* name = reinterpret_cast<const char*>(h + stroff + strx);
            require(std::memchr(name, 0, strsize - strx) != nullptr, "unterminated symbol table string");
            void* address = nullptr;
            if ((type & 0xe) == 0xe) {
                address = loaded.address_for_vmaddr(value, 0);
                require(address, "defined symbol outside image");
            } else if ((type & 0xe) == 2) address = reinterpret_cast<void*>(value);
            if (address) loaded.symbols.emplace(name, address);
        }
        Fixups fixups{loaded, resolver, user};
        fixups.rebase(bytes, rebase);
        fixups.bind(bytes, bind, false, false);
        fixups.bind(bytes, weak, false, true);
        fixups.bind(bytes, lazy, true, false); // Eagerly resolve all lazy pointers.
        for (const auto& section : loaded.sections) if ((section.flags & 0xff) == 9 || section.name == "__mod_init_func") {
            require((section.size & 7) == 0, "mis-sized initializer pointer section");
            for (size_t i = 0; i < section.size; i += 8) {
                uintptr_t value = u64(static_cast<const uint8_t*>(section.address) + i);
                bool executable = false;
                for (const auto& s : loaded.segments) {
                    uintptr_t start = reinterpret_cast<uintptr_t>(s.address);
                    if (s.address && (s.protection & PROT_EXEC) && value >= start && range(value - start, 4, s.vmsize)) executable = true;
                }
                require(executable && !(value & 3), "initializer pointer outside executable segment");
                loaded.mod_init_functions.push_back(reinterpret_cast<void*>(value));
            }
        }
        for (const auto& s : loaded.segments) if (s.address) {
            if (s.protection & PROT_EXEC) __builtin___clear_cache(static_cast<char*>(s.address), static_cast<char*>(s.address) + s.vmsize);
            require(mprotect(s.address, size_t(s.vmsize), s.protection) == 0,
                    "mprotect final segment: " + std::string(std::strerror(errno)));
        }
        image = std::move(loaded);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        macho_unload(loaded);
        return false;
    }
}
