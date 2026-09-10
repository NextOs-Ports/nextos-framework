#ifndef _PIKI_PORT_BYTESWAP_HPP
#define _PIKI_PORT_BYTESWAP_HPP

#include <cstdint>
#include <cstring>

namespace piki::swap {

inline uint16_t be16(uint16_t v) { return __builtin_bswap16(v); }
inline uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }
inline uint64_t be64(uint64_t v) { return __builtin_bswap64(v); }

inline float bef32(float v)
{
	uint32_t bits;
	std::memcpy(&bits, &v, sizeof(bits));
	bits = __builtin_bswap32(bits);
	std::memcpy(&v, &bits, sizeof(v));
	return v;
}

// In-place fixups for the common shapes.
inline void be16v(void* data, size_t count)
{
	auto* p = static_cast<uint16_t*>(data);
	for (size_t i = 0; i < count; i++) {
		p[i] = be16(p[i]);
	}
}

inline void be32v(void* data, size_t count)
{
	auto* p = static_cast<uint32_t*>(data);
	for (size_t i = 0; i < count; i++) {
		p[i] = be32(p[i]);
	}
}

} // namespace piki::swap

#endif
