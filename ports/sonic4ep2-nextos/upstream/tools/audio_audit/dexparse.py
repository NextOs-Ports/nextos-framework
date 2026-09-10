#!/usr/bin/env python3
# Parser minimo de DEX: extrai os pares (key, value) dos metodos GetCueMap_* da
# classe com/mineloader/fox/AudioDataTbl, lendo os const-string em ordem.
import struct, sys

DEX = sys.argv[1]
data = open(DEX, "rb").read()

def u32(o): return struct.unpack_from("<I", data, o)[0]
def u16(o): return struct.unpack_from("<H", data, o)[0]

# header offsets
string_ids_size = u32(0x38); string_ids_off = u32(0x3c)
type_ids_size   = u32(0x40); type_ids_off   = u32(0x44)
method_ids_size = u32(0x58); method_ids_off = u32(0x5c)
class_defs_size = u32(0x60); class_defs_off = u32(0x64)

def uleb(o):
    res = 0; sh = 0
    while True:
        b = data[o]; o += 1
        res |= (b & 0x7f) << sh
        if not (b & 0x80): break
        sh += 7
    return res, o

def get_string(idx):
    off = u32(string_ids_off + idx*4)
    _, p = uleb(off)              # utf16 size (skip)
    end = data.index(b"\x00", p)
    return data[p:end].decode("utf-8", "replace")

def type_descriptor(tidx):
    return get_string(u32(type_ids_off + tidx*4))

# method_id -> (class_idx, name_str)
def method_name(midx):
    base = method_ids_off + midx*8
    cls = u16(base); name_idx = u32(base+4)
    return cls, get_string(name_idx)

# find AudioDataTbl class_def
target = "Lcom/mineloader/fox/AudioDataTbl;"
cd_off = None
for i in range(class_defs_size):
    b = class_defs_off + i*32
    cidx = u32(b)
    if type_descriptor(cidx) == target:
        cd_off = b; break
if cd_off is None:
    print("AudioDataTbl class nao encontrada"); sys.exit(1)

class_data_off = u32(cd_off + 24)
if class_data_off == 0:
    print("sem class_data"); sys.exit(1)

# class_data_item
o = class_data_off
sf, o = uleb(o); inf, o = uleb(o); dm, o = uleb(o); vm, o = uleb(o)
# skip encoded_field arrays
for _ in range(sf):
    _, o = uleb(o); _, o = uleb(o)
for _ in range(inf):
    _, o = uleb(o); _, o = uleb(o)

# dalvik instruction widths (code units) per opcode 0x00-0xFF
W = [1]*256
def setw(lst, w):
    for op in lst: W[op] = w
# 2-unit
setw([0x20,0x22,0x23,0x21,0x24,0x25,0x60,0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x6b,0x6c,0x6d,0x6e,0x6f,0x70,0x71,0x72,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a], 2)
setw(list(range(0x90,0xe3)), 2)
setw([0x1a,0x1c,0x1d,0x1e,0x1f,0x52,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x5b,0x5c,0x5d,0x5e,0x5f,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x4b,0x4c,0x4d,0x4e,0x4f,0x50,0x51], 2)
setw([0x13,0x15,0x16,0x19,0x14], 2); W[0x14]=3; W[0x17]=3
# 3-unit
setw([0x1b,0x26,0x2a,0x2b,0x2c,0x6e,0x6f,0x70,0x71,0x72,0x74,0x75,0x76,0x77,0x78,0x24,0x25,0x14,0x17], 3)
setw([0x6e,0x6f,0x70,0x71,0x72], 3)
W[0x18]=5  # const-wide 51l
W[0x00]=1
# const-string=0x1a (2 units), const-string/jumbo=0x1b (3 units)
W[0x1a]=2; W[0x1b]=3

def walk_method(code_off):
    """Coleta const-strings na ordem do bytecode (walk com largura correta).
    A tabela e construida como pares (cue, arquivo) consecutivos."""
    if code_off == 0: return []
    insns_size = u32(code_off + 12)
    base = code_off + 16
    strs = []
    i = 0
    while i < insns_size:
        unit = u16(base + i*2)
        op = unit & 0xff
        if op == 0x00 and unit != 0x0000:
            ident = unit
            if ident == 0x0100:
                size = u16(base+(i+1)*2); i += 2 + size*2
            elif ident == 0x0200:
                size = u16(base+(i+1)*2); i += 2 + size*4
            elif ident == 0x0300:
                ew = u16(base+(i+1)*2); sz = u32(base+(i+2)*2)
                i += 4 + (sz*ew + 1)//2
            else:
                i += 1
            continue
        if op == 0x1a:
            strs.append(get_string(u16(base+(i+1)*2)))
        elif op == 0x1b:
            strs.append(get_string(u32(base+(i+1)*2)))
        i += W[op] if W[op] > 0 else 1
    return strs

# walk direct+virtual methods, find GetCueMap_*
def read_methods(count, o):
    out = []
    prev = 0
    for _ in range(count):
        diff, o = uleb(o); acc, o = uleb(o); coff, o = uleb(o)
        prev += diff
        out.append((prev, coff))
    return out, o

dmeth, o = read_methods(dm, o)
vmeth, o = read_methods(vm, o)

BANK = {
 "GetCueMap_S4EP2Default":"default",
 "GetCueMap_S4EP1_SND_SE":"ep1",
 "GetCueMap_S4EP2_SND_SE":"ep2",
 "GetCueMap_S4EP2_SND_SE_Z1":"ep2zone1",
 "GetCueMap_S4EP2_SND_SE_Z2":"ep2zone2",
 "GetCueMap_S4EP2_SND_SE_Z3":"ep2zone3",
 "GetCueMap_S4EP2_SND_SE_Z4":"ep2zone4",
 "GetCueMap_S4EP2_SND_SE_ZF":"ep2zonef",
 "GetCueMap_S4EP2_SND_SNG":"ep2sng",
}

rows = []
for midx, coff in dmeth + vmeth:
    _, name = method_name(midx)
    if name in BANK:
        strs = walk_method(coff)
        bank = BANK[name]
        for k in range(0, len(strs)-1, 2):
            rows.append((bank, strs[k], strs[k+1]))
        print(f"# {name} ({bank}): {len(strs)} const-strings -> {len(strs)//2} pares", file=sys.stderr)

for b,k,v in rows:
    print(f"{b}\t{k}\t{v}")
print(f"# TOTAL pares={len(rows)} cues_unicas={len({r[1] for r in rows})}", file=sys.stderr)
