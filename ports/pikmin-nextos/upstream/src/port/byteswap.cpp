// Endianness.
//
// Everything the game reads off the disc was authored for a big-endian CPU.
// Aurora already decodes GX vertex arrays and textures in place, so the work
// left here is the data the *CPU* reads: file headers, model tables, parameter
// blocks.  Each converter lives next to the loader that needs it, and is added
// only once the loader is proven to be the thing reading a wrong value - a
// blanket swap of everything would corrupt the buffers Aurora reads raw.

#include "port/byteswap.hpp"

namespace piki::swap {

// (converters are added per format as the boot path reaches them)

} // namespace piki::swap
