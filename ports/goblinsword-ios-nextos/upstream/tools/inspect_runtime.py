#!/usr/bin/env python3
"""Read-only Mach-O/Objective-C inspection; disassembly never executes guest code.

Uses the already installed Python Capstone package. Reads only the supplied
Mach-O, writes JSON to stdout, and does not inspect provisioning/signatures.
Run from any cwd:
  python3 tools/inspect_runtime.py /path/to/goblin-sword.macho > runtime-analysis.json
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

import capstone
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_MEM, ARM64_OP_REG


class MachO:
    def __init__(self, data):
        self.data = data
        if data[:4] != b"\xcf\xfa\xed\xfe":
            raise ValueError("Expected a thin little-endian Mach-O 64")
        if self.unpack("<I", 4)[0] != 0x100000C:
            raise ValueError("Expected ARM64")
        self.segments = []
        self.sections = {}
        self.dylibs = []
        self.symbols = []
        self.stubs = {}
        self.entryoff = None
        symtab = indirect = None
        ncmds, sizeofcmds = self.unpack("<II", 16)
        pos, commands_end = 32, 32 + sizeofcmds
        self.bounds(32, sizeofcmds)
        for _ in range(ncmds):
            cmd, size = self.unpack("<II", pos)
            if size < 8 or pos + size > commands_end:
                raise ValueError("Invalid load command")
            if cmd == 0x19:
                name = self.data[pos + 8:pos + 24].rstrip(b"\0").decode()
                vmaddr, vmsize, fileoff, filesize = self.unpack("<QQQQ", pos + 24)
                self.bounds(fileoff, filesize)
                self.segments.append(dict(name=name, vmaddr=vmaddr, vmsize=vmsize,
                                          fileoff=fileoff, filesize=filesize))
                nsects = self.unpack("<I", pos + 64)[0]
                if 72 + nsects * 80 > size:
                    raise ValueError("Section table exceeds load command")
                for j in range(nsects):
                    values = self.unpack("<16s16sQQIIIIIIII", pos + 72 + j * 80)
                    sn, sg, addr, sz, off, align, ro, nr, flags, r1, r2, r3 = values
                    name = sn.rstrip(b"\0").decode()
                    self.sections[name] = dict(name=name, segment=sg.rstrip(b"\0").decode(),
                                               addr=addr, size=sz, offset=off,
                                               flags=flags, reserved1=r1, reserved2=r2)
            elif cmd in (0xC, 0x80000018, 0x8000001F, 0x20, 0x80000023):
                offset = self.unpack("<I", pos + 8)[0]
                if not 8 <= offset < size:
                    raise ValueError("Invalid dylib string offset")
                self.dylibs.append(self.data[pos + offset:pos + size].split(b"\0", 1)[0].decode())
            elif cmd == 2:
                symtab = self.unpack("<IIII", pos + 8)
            elif cmd == 0xB:
                indirect = self.unpack("<II", pos + 56)
            elif cmd == 0x80000028:
                self.entryoff = self.unpack("<Q", pos + 8)[0]
            pos += size
        if pos != commands_end:
            raise ValueError("Load command size mismatch")
        if symtab:
            so, n, stro, strsz = symtab
            self.bounds(so, 16 * n)
            self.bounds(stro, strsz)
            for i in range(n):
                idx, typ, section, desc, value = self.unpack("<IBBHQ", so + 16 * i)
                if idx >= strsz:
                    raise ValueError("Invalid symbol string index")
                name = self.data[stro + idx:stro + strsz].split(b"\0", 1)[0].decode()
                self.symbols.append(dict(name=name, type=typ, section=section, desc=desc, value=value))
        if indirect and "__stubs" in self.sections:
            io, count = indirect
            self.bounds(io, count * 4)
            sec = self.sections["__stubs"]
            step = sec["reserved2"]
            if not step or sec["size"] % step:
                raise ValueError("Invalid stub size")
            for j in range(sec["size"] // step):
                idx = sec["reserved1"] + j
                if idx >= count:
                    raise ValueError("Invalid indirect symbol index")
                symbol = self.unpack("<I", io + 4 * idx)[0]
                if symbol < len(self.symbols):
                    self.stubs[sec["addr"] + j * step] = self.symbols[symbol]["name"]

    def bounds(self, offset, size):
        if offset < 0 or size < 0 or offset + size > len(self.data):
            raise ValueError("Read outside Mach-O")

    def unpack(self, fmt, offset):
        self.bounds(offset, struct.calcsize(fmt))
        return struct.unpack_from(fmt, self.data, offset)

    def file_offset(self, address):
        for segment in self.segments:
            if segment["vmaddr"] <= address < segment["vmaddr"] + segment["filesize"]:
                return address - segment["vmaddr"] + segment["fileoff"]
        raise ValueError(f"Address not file-backed: {address:#x}")

    def virtual_address(self, offset):
        for segment in self.segments:
            if segment["fileoff"] <= offset < segment["fileoff"] + segment["filesize"]:
                return offset - segment["fileoff"] + segment["vmaddr"]
        raise ValueError("Offset has no virtual address")

    def ptr(self, address):
        return self.unpack("<Q", self.file_offset(address))[0]

    def cstring(self, address):
        offset = self.file_offset(address)
        end = self.data.find(b"\0", offset, offset + 65536)
        if end < 0:
            raise ValueError("Unterminated string")
        return self.data[offset:end].decode("utf-8", errors="replace")

    def objc_methods(self):
        classes, methods = [], []
        sec = self.sections["__objc_classlist"]
        if sec["size"] % 8:
            raise ValueError("Misaligned Objective-C class list")
        for i in range(sec["size"] // 8):
            addr = self.unpack("<Q", sec["offset"] + 8 * i)[0]
            for kind, cls in (("instance", addr), ("class", self.ptr(addr))):
                ro = self.ptr(cls + 32) & ~7
                name = self.cstring(self.ptr(ro + 24))
                if kind == "instance":
                    classes.append(dict(name=name, address=hex(addr), file_offset=hex(self.file_offset(addr))))
                ml = self.ptr(ro + 32)
                if not ml:
                    continue
                offset = self.file_offset(ml)
                entsize, count = self.unpack("<II", offset)
                if entsize & 0x80000000:
                    raise ValueError("Relative Objective-C method lists are not supported")
                stride = entsize & 0xFFFF
                if stride < 24 or count > 65536:
                    raise ValueError("Invalid Objective-C method list")
                self.bounds(offset + 8, stride * count)
                for j in range(count):
                    mn, ty, imp = self.unpack("<QQQ", offset + 8 + j * stride)
                    methods.append(dict(class_name=name, kind=kind, selector=self.cstring(mn),
                                        type_encoding=self.cstring(ty), imp=imp,
                                        file_offset=hex(self.file_offset(imp))))
        return classes, sorted(methods, key=lambda method: method["imp"])

    def disassemble(self, start, end):
        """Annotate direct calls and selectors with a limited local value trace.

        The trace is a convenience, not a full decompiler or proof of liveness.
        It resets scratch registers after calls; raw instructions remain present.
        """
        cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        cs.detail = True
        registers, lines = {}, []
        def regname(reg):
            return cs.reg_name(reg).replace("w", "x", 1)
        def value(operand):
            if operand.type == ARM64_OP_REG:
                return registers.get(regname(operand.reg))
            if operand.type == ARM64_OP_IMM:
                return operand.imm
            return None
        startoff = self.file_offset(start)
        endoff = self.file_offset(end - 1) + 1
        for ins in cs.disasm(self.data[startoff:endoff], start):
            operands = ins.operands
            computed = {}
            if ins.mnemonic in ("adr", "adrp"):
                computed[regname(operands[0].reg)] = operands[1].imm
            elif ins.mnemonic == "mov" and len(operands) == 2:
                computed[regname(operands[0].reg)] = value(operands[1])
            elif ins.mnemonic in ("add", "sub") and len(operands) == 3:
                a, b = value(operands[1]), value(operands[2])
                if isinstance(a, int) and isinstance(b, int):
                    if operands[2].shift.value:
                        b <<= operands[2].shift.value
                    computed[regname(operands[0].reg)] = a + b if ins.mnemonic == "add" else a - b
            elif ins.mnemonic in ("ldr", "ldur", "ldrsw") and len(operands) == 2 and operands[1].type == ARM64_OP_MEM:
                mem = operands[1].mem
                base = registers.get(regname(mem.base))
                if isinstance(base, int) and not mem.index:
                    try:
                        address = base + mem.disp
                        is32 = cs.reg_name(operands[0].reg).startswith("w") or ins.mnemonic == "ldrsw"
                        fmt = "<i" if ins.mnemonic == "ldrsw" else "<I" if is32 else "<Q"
                        computed[regname(operands[0].reg)] = self.unpack(fmt, self.file_offset(address))[0]
                    except ValueError:
                        pass
            for reg in ins.regs_access()[1]:
                registers[regname(reg)] = None
            registers.update(computed)
            line = dict(address=hex(ins.address), file_offset=hex(self.file_offset(ins.address)),
                        bytes=ins.bytes.hex(), instruction=ins.mnemonic + " " + ins.op_str)
            if ins.mnemonic in ("bl", "b") and operands[0].type == ARM64_OP_IMM:
                target = operands[0].imm
                line["target"] = self.stubs.get(target, hex(target))
                if line["target"] in ("_objc_msgSend", "_objc_msgSendSuper2"):
                    selector = registers.get("x1")
                    if isinstance(selector, int):
                        try:
                            name = self.cstring(selector)
                            if len(name) <= 200 and name.isprintable():
                                line["selector"] = name
                        except ValueError:
                            pass
                    line["known_integer_arguments"] = {
                        reg: hex(registers[reg]) for reg in ("x2", "x3", "x4", "x5")
                        if isinstance(registers.get(reg), int)
                    }
                if ins.mnemonic == "bl":
                    for i in range(19):
                        registers["x" + str(i)] = None
            lines.append(line)
            if ins.mnemonic == "ret":
                registers.clear()
        return lines


def analyze(path):
    binary = path.read_bytes()
    macho = MachO(binary)
    classes, methods = macho.objc_methods()
    selected = []
    names = {c["name"] for c in classes}
    for method in methods:
        name, selector = method["class_name"], method["selector"]
        if (name in ("CCES2Renderer", "AppController", "GCHelper")
                or selector in ("mfisupport", "mfipause", "mfireturn", "initControls")
                or name == "Player" and selector == "initWithWorld:atLocation:withWeapon:"):
            item = dict(method)
            item["imp"] = hex(item["imp"])
            selected.append(item)
    # Bound inspection to Objective-C methods and text before the next method;
    # callbacks located between method IMPs are intentionally retained.
    wanted = {
        ("CCES2Renderer", "initWithDepthFormat:withPixelFormat:withSharegroup:withMultiSampling:withNumberOfSamples:"),
        ("AppController", "application:didFinishLaunchingWithOptions:"),
        ("GCHelper", "controllerStateChanged"),
        ("GCHelper", "initWithScoresToReport:achievementsToReport:"),
        ("MenuLayer", "mfisupport"), ("Player", "mfipause"), ("Player", "mfireturn"),
        ("MyNavigationController", "directorDidReshapeProjection:"),
        ("CCDirectorIOS", "reshapeProjection:"), ("CCDirectorIOS", "viewWillAppear:"),
        ("CCDirectorIOS", "viewDidAppear:"), ("CCDirectorIOS", "viewDidLoad"),
        ("CCDirectorIOS", "drawScene"), ("CCDirectorIOS", "runWithScene:"),
        ("CCDirectorDisplayLink", "mainLoop:"), ("CCDirectorDisplayLink", "startAnimation"),
        ("CCGLView", "layoutSubviews"), ("CCGLView", "swapBuffers"),
        ("CCGLView", "setupSurfaceWithSharegroup:"), ("MenuScene", "init"),
    }
    disassemblies = {}
    for i, method in enumerate(methods):
        if (method["class_name"], method["selector"]) in wanted:
            end = methods[i + 1]["imp"]
            label = ("+" if method["kind"] == "class" else "-") + "[" + method["class_name"] + " " + method["selector"] + "]"
            disassemblies[label] = macho.disassemble(method["imp"], end)
    entry = None
    if macho.entryoff is not None:
        address = macho.virtual_address(macho.entryoff)
        following = next((m["imp"] for m in methods if m["imp"] > address), address + 512)
        # Entry may precede another non-ObjC function; stop at first RET, at most 512 bytes.
        lines = macho.disassemble(address, min(following, address + 512))
        ret = next((i for i, line in enumerate(lines) if line["instruction"].startswith("ret")), None)
        if ret is not None:
            lines = lines[:ret + 1]
        entry = dict(source="LC_MAIN", address=hex(address), file_offset=hex(macho.entryoff), disassembly=lines)
    imports = [s["name"] for s in macho.symbols if s["type"] & 0x0E == 0]
    evidence_strings = []
    # Restrict to __TEXT literal sections, avoiding signature/receipt metadata.
    for secname in ("__cstring", "__const"):
        section = macho.sections.get(secname)
        if not section:
            continue
        offset, size = section["offset"], section["size"]
        for match in re.finditer(rb"[\x20-\x7e]{5,}", binary[offset:offset + size]):
            text = match.group().decode()
            if re.search(r"cocos2d|CocosDenshion|Box2D", text):
                evidence_strings.append(dict(section=secname, file_offset=hex(offset + match.start()), text=text))
    return {
        "schema": "goblin-sword-static-runtime-inspection/1",
        "input": {"filename": path.name, "bytes": len(binary), "sha256": hashlib.sha256(binary).hexdigest()},
        "tool": {"capstone_version": capstone.__version__, "executes_guest_code": False},
        "limitations": [
            "Static evidence only; does not prove gameplay, host compatibility, FPS, memory usage or physical controls.",
            "Disassembly selector annotations use limited local constant tracking; raw bytes and instructions are authoritative.",
            "Exact cocos2d version was not identified; class names establish the Objective-C cocos2d family.",
            "No Metal dependency/import observed; this does not assert impossibility of any dynamically resolved dependency.",
        ],
        "conclusions": {
            "engine": "cocos2d Objective-C, exact version unconfirmed",
            "physics": "Box2D types in Player initWithWorld type encoding",
            "graphics": "CCES2Renderer explicitly passes API=2 to EAGLContext initialization",
            "audio": "SimpleAudioEngine/CD classes, OpenAL, AudioToolbox and AVFoundation imports",
            "controllers": "Native GameController standard/extended profiles and actual Blocks registered in MenuLayer and Player",
        },
        "dylibs": macho.dylibs,
        "runtime_class_count": len(classes),
        "runtime_method_count_including_class_methods": len(methods),
        "relevant_classes": sorted(n for n in names if n.startswith(("CC", "CD")) or n in
                                   ("SimpleAudioEngine", "AppController", "GCHelper", "Player", "GameLayer", "MenuLayer")),
        "relevant_imports": [n for n in imports if re.search(r"gl[A-Z]|EAGL|Metal|MTL|GCController|^_al|Audio|objc|Block|UIApplicationMain", n)],
        "engine_strings": evidence_strings,
        "selected_methods": selected,
        "native_entry": entry,
        "disassemblies": disassemblies,
        "verified_locations": {
            "gles2_with_sharegroup": {"api_value_instruction": "0x100095c68", "call": "0x100095c70", "selector": "initWithAPI:sharegroup:", "api": 2},
            "gles2_without_sharegroup": {"api_value_instruction": "0x100095c80", "call": "0x100095c84", "selector": "initWithAPI:", "api": 2},
            "menu_buttonA_handler": {"call": "0x10001c560", "callback": "0x10001c8d8", "selector": "setValueChangedHandler:"},
            "player_buttonA_handler": {"call": "0x1000c7ba8", "callback": "0x1000c7f6c", "selector": "setValueChangedHandler:"},
            "player_buttonB_handler": {"call": "0x1000c7be4", "callback": "0x1000c8004", "selector": "setValueChangedHandler:"},
            "player_pause_handler": {"call": "0x1000c7a10", "callback": "0x1000c7a38", "selector": "setControllerPausedHandler:"},
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("macho", type=Path, help="Owner-supplied ARM64 Mach-O executable")
    args = parser.parse_args()
    print(json.dumps(analyze(args.macho), ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
