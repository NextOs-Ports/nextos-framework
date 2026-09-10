#!/usr/bin/env python3
"""Regression test for the macro-only undefined-symbol gate (D1).

A loader that imports an SDL macro (e.g. Mix_PlayChannel) as an UNDEFINED
dynamic symbol crashes with `undefined symbol` on devices whose library only
exports the real function (Mix_PlayChannelTimed). Magic Rampage v1.1.3 shipped
exactly this and died on the first sound (spruce/Mali-G52, status 127). The gate
must reject that at build time. This test drives the pure parser with the same
`readelf --dyn-syms --wide` text layout the real check consumes.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import nxrelease as nx


def rows(*lines):
    header = (
        "\nSymbol table '.dynsym' contains 9 entries:\n"
        "   Num:    Value          Size Type    Bind   Vis      Ndx Name\n"
    )
    return header + "\n".join(lines) + "\n"


def check(label, condition):
    if not condition:
        raise SystemExit("macro-dynsym gate test FAIL: " + label)


# The exact bug: Mix_PlayChannel imported as UND.
bad = rows(
    "    64: 0000000000000000     0 FUNC    GLOBAL DEFAULT  UND Mix_PlayChannel",
    "    65: 0000000000000000     0 FUNC    GLOBAL DEFAULT  UND SDL_NumJoysticks",
)
check("catches UND Mix_PlayChannel",
      nx.macro_only_undefined_symbols(bad) == ["Mix_PlayChannel"])

# The fix: only the real symbol is imported -> clean.
good = rows(
    "    64: 0000000000000000     0 FUNC    GLOBAL DEFAULT  UND Mix_PlayChannelTimed",
    "    65: 0000000000000000     0 FUNC    GLOBAL DEFAULT  UND SDL_NumJoysticks",
)
check("passes Mix_PlayChannelTimed", nx.macro_only_undefined_symbols(good) == [])

# A macro name that is DEFINED (not UND) is fine -- some lib could export it.
defined = rows(
    "    64: 0000000000012340    16 FUNC    GLOBAL DEFAULT   12 SDL_BlitSurface",
)
check("ignores defined macro name", nx.macro_only_undefined_symbols(defined) == [])

# Versioned undefined symbol (Name@VER) must still be caught by base name.
versioned = rows(
    "    64: 0000000000000000     0 FUNC    GLOBAL DEFAULT  UND SDL_LoadBMP@SDL2",
)
check("catches versioned UND macro",
      nx.macro_only_undefined_symbols(versioned) == ["SDL_LoadBMP"])

# Multiple distinct macro imports, de-duplicated and order-preserving.
multi = rows(
    "    64: 0 0 FUNC GLOBAL DEFAULT UND SDL_BlitScaled",
    "    65: 0 0 FUNC GLOBAL DEFAULT UND SDL_BlitScaled",
    "    66: 0 0 FUNC GLOBAL DEFAULT UND Mix_LoadWAV",
)
check("dedups and preserves order",
      nx.macro_only_undefined_symbols(multi) == ["SDL_BlitScaled", "Mix_LoadWAV"])

# Every denylisted name maps to a DIFFERENT real symbol (never itself).
for macro, real in nx.MACRO_ONLY_DYNSYMS.items():
    check("maps {} to a different real symbol".format(macro), macro != real)

print("nxrelease macro-dynsym gate test: PASS cases=6 denylist={}".format(
    len(nx.MACRO_ONLY_DYNSYMS)))
