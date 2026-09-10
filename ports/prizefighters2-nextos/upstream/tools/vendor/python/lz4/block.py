"""ctypes-backed subset of :mod:`lz4.block` for PF2 data preparation.

The release bundles the audited AArch64 LZ4 runtime also used by the approved
Stardew Valley universal extractor.  This wrapper supports exactly the block
operations UnityPy needs for UnityFS and ShaderProgram data.
"""

from __future__ import absolute_import

import ctypes
import os


class LZ4BlockError(RuntimeError):
    pass


_library = None


def _load_library():
    global _library
    if _library is not None:
        return _library

    candidates = []
    override = os.environ.get("PF2_LZ4_LIBRARY")
    if override:
        candidates.append(override)
    candidates.extend(("liblz4.so.1", "liblz4.so"))
    errors = []
    for candidate in candidates:
        try:
            library = ctypes.CDLL(candidate)
        except OSError as error:
            errors.append("%s: %s" % (candidate, error))
            continue
        library.LZ4_decompress_safe.argtypes = (
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_int,
        )
        library.LZ4_decompress_safe.restype = ctypes.c_int
        library.LZ4_compressBound.argtypes = (ctypes.c_int,)
        library.LZ4_compressBound.restype = ctypes.c_int
        library.LZ4_compress_HC.argtypes = (
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
        )
        library.LZ4_compress_HC.restype = ctypes.c_int
        _library = library
        return library
    raise LZ4BlockError("no usable LZ4 library (%s)" % "; ".join(errors))


def decompress(data, uncompressed_size=0, return_bytearray=False, **_kwargs):
    if not isinstance(data, (bytes, bytearray, memoryview)):
        raise TypeError("a bytes-like object is required")
    source = bytes(data)
    if uncompressed_size <= 0:
        raise LZ4BlockError("PF2 requires the explicit uncompressed block size")
    library = _load_library()
    source_buffer = ctypes.create_string_buffer(source, len(source))
    output_buffer = ctypes.create_string_buffer(uncompressed_size)
    result = library.LZ4_decompress_safe(
        source_buffer,
        output_buffer,
        len(source),
        uncompressed_size,
    )
    if result != uncompressed_size:
        raise LZ4BlockError(
            "decompression produced %d bytes, expected %d"
            % (result, uncompressed_size)
        )
    output = output_buffer.raw[:uncompressed_size]
    return bytearray(output) if return_bytearray else output


def compress(
    data,
    mode="default",
    acceleration=1,
    compression=0,
    store_size=True,
    return_bytearray=False,
    **_kwargs
):
    del mode, acceleration
    if not isinstance(data, (bytes, bytearray, memoryview)):
        raise TypeError("a bytes-like object is required")
    source = bytes(data)
    library = _load_library()
    bound = library.LZ4_compressBound(len(source))
    if bound <= 0:
        raise LZ4BlockError("LZ4 rejected the input size")
    source_buffer = ctypes.create_string_buffer(source, len(source))
    output_buffer = ctypes.create_string_buffer(bound)
    level = int(compression) if compression else 9
    result = library.LZ4_compress_HC(
        source_buffer,
        output_buffer,
        len(source),
        bound,
        level,
    )
    if result <= 0:
        raise LZ4BlockError("LZ4 compression failed")
    output = output_buffer.raw[:result]
    if store_size:
        output = len(source).to_bytes(4, "little") + output
    return bytearray(output) if return_bytearray else output
