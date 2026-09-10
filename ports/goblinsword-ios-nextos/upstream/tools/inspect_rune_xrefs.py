#!/usr/bin/env python3
"""Locate Darwin rune data imports and direct address references, read-only."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

import capstone
from capstone import arm64
from inspect_runtime import MachO


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--macho', required=True, type=Path, help='Owner-supplied ARM64 Mach-O executable')
    path = parser.parse_args().macho
    macho = MachO(path.read_bytes())
    pos = 32
    indirect = None
    for _ in range(struct.unpack_from("<I", macho.data, 16)[0]):
        command, size = struct.unpack_from("<II", macho.data, pos)
        if command == 0xB:
            indirect, count = struct.unpack_from("<II", macho.data, pos + 56)
        pos += size
    if indirect is None:
        raise ValueError("Missing indirect symbol table")
    bindings = {}
    for section in macho.sections.values():
        if section["flags"] & 0xFF not in (6, 7):
            continue
        for i in range(section["size"] // 8):
            index = section["reserved1"] + i
            if index >= count:
                raise ValueError("Indirect symbol index outside table")
            symbol = struct.unpack_from("<I", macho.data, indirect + 4 * index)[0]
            if symbol < len(macho.symbols):
                name = macho.symbols[symbol]["name"]
                if "RuneLocale" in name:
                    bindings[section["addr"] + 8 * i] = name
    decoder = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    decoder.detail = True
    # Literal/data words occur inside __text. A default Capstone iterator
    # stops at the first invalid instruction and would leave later code unseen.
    decoder.skipdata = True
    registers, hits = {}, []
    skipped = 0
    last = None
    text = macho.sections["__text"]
    body = macho.data[text["offset"]:text["offset"] + text["size"]]
    for ins in decoder.disasm(body, text["addr"]):
        last = ins.address
        if not ins.id:
            skipped += len(ins.bytes)
            registers.clear()
            continue
        operands, updates = ins.operands, {}
        if ins.mnemonic in ("adr", "adrp"):
            updates[operands[0].reg] = operands[1].imm
        elif ins.mnemonic == "mov" and len(operands) == 2 and operands[1].type == arm64.ARM64_OP_REG:
            if operands[1].reg in registers:
                updates[operands[0].reg] = registers[operands[1].reg]
        elif ins.mnemonic == "add" and len(operands) == 3 and operands[2].type == arm64.ARM64_OP_IMM:
            if operands[1].reg in registers:
                updates[operands[0].reg] = registers[operands[1].reg] + (operands[2].imm << operands[2].shift.value)
        for operand in operands:
            if operand.type != arm64.ARM64_OP_MEM or operand.mem.base not in registers or operand.mem.index:
                continue
            address = registers[operand.mem.base] + operand.mem.disp
            candidates = (address, address + 8) if ins.mnemonic == "ldp" else (address,)
            for candidate in candidates:
                if candidate in bindings:
                    hits.append({"address": hex(ins.address), "file_offset": hex(macho.file_offset(ins.address)),
                                 "instruction": ins.mnemonic + " " + ins.op_str, "symbol": bindings[candidate],
                                 "disassembly": macho.disassemble(ins.address - 16, ins.address + 128)})
        for register in ins.regs_access()[1]:
            registers.pop(register, None)
        registers.update(updates)
        if ins.mnemonic in ("bl", "blr"):
            for i in range(19):
                registers.pop(getattr(arm64, "ARM64_REG_X" + str(i)), None)
    print(json.dumps({"schema": "goblin-rune-direct-xrefs/1", "sha256": hashlib.sha256(macho.data).hexdigest(),
                      "bindings": {hex(key): value for key, value in bindings.items()},
                      "text_start": hex(text["addr"]), "text_size": text["size"], "last_instruction": hex(last),
                      "skipped_data_bytes": skipped, "hits": hits,
                      "limitation": "Limited constant-address trace; zero hits does not prove the import unused or exclude indirect/complex address calculations."}, indent=2))


if __name__ == "__main__":
    main()
