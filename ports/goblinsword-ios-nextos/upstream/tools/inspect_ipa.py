#!/usr/bin/env python3
"""Static IPA inventory for the study. Never runs guest code or extracts a ZIP tree.

Mach-O scope: one thin little-endian ARM64 executable. Other formats fail explicitly.
Only selected Info.plist fields are exported; provisioning/receipts are not read.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path, PurePosixPath
import plistlib
import re
import struct
import zipfile


def sha(data):
    return hashlib.sha256(data).hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def version(value):
    return '.'.join(str(v) for v in (value >> 16, (value >> 8) & 255, value & 255))


class MachO:
    def __init__(self, data):
        self.data = data
        self.bounds(0, 32)
        h = self.unpack('<8I', 0)
        require(h[0] == 0xfeedfacf and h[1] == 0x0100000c,
                'Only thin little-endian ARM64 Mach-O is supported by this study tool')
        self.header = dict(zip(('magic', 'cpu_type', 'cpu_subtype', 'file_type',
                               'command_count', 'command_bytes', 'flags', 'reserved'), h))
        self.bounds(32, h[5])
        require(h[4] <= h[5] // 8, 'Impossible load command count')
        self.commands, self.segments, self.sections, self.libraries = [], [], [], []
        self.encryption = []
        self.symbols = []
        self.dyld_info = None
        self.ios_version = None
        self.entry_offset = None
        symtab = None
        off, end = 32, 32 + h[5]
        for _ in range(h[4]):
            self.bounds(off, 8)
            cmd, size = self.unpack('<II', off)
            require(size >= 8 and size % 8 == 0 and off + size <= end,
                    f'Invalid load command at {off:#x}')
            self.commands.append({'offset': off, 'command': hex(cmd), 'size': size})
            if cmd == 0x19:
                require(size >= 72, 'Short segment command')
                s = self.unpack('<II16sQQQQiiII', off)
                require(72 + s[9] * 80 <= size, 'Sections exceed segment command')
                self.bounds(s[5], s[6])
                require(s[6] <= s[4], 'File segment exceeds VM segment')
                segment = dict(zip(('name', 'vm_address', 'vm_size', 'file_offset',
                                    'file_size', 'max_protection', 'initial_protection',
                                    'section_count', 'flags'),
                                   (s[2].rstrip(b'\0').decode(), *s[3:])))
                self.segments.append(segment)
                for j in range(s[9]):
                    q = self.unpack('<16s16sQQIIIIIIII', off + 72 + 80*j)
                    section = dict(zip(('name', 'segment', 'address', 'size', 'offset',
                                        'alignment', 'relocation_offset', 'relocation_count',
                                        'flags', 'reserved1', 'reserved2', 'reserved3'),
                                       (q[0].rstrip(b'\0').decode(), q[1].rstrip(b'\0').decode(), *q[2:])))
                    if (q[8] & 255) not in (1, 0xC, 0x12):
                        self.bounds(q[4], q[3])
                    self.sections.append(section)
            elif cmd in (0xC, 0x80000018, 0x8000001F, 0x80000023):
                require(size >= 24, 'Short dylib command')
                n, timestamp, current, compat = self.unpack('<4I', off+8)
                require(24 <= n < size, 'Invalid dylib name offset')
                self.libraries.append({'ordinal': len(self.libraries)+1,
                                       'path': self.string(off+n, off+size),
                                       'weak': cmd == 0x80000018,
                                       'current_version': version(current),
                                       'compatibility_version': version(compat)})
            elif cmd == 0x2:
                require(size >= 24, 'Short symtab command')
                symtab = self.unpack('<4I', off+8)
            elif cmd in (0x21, 0x2C):
                require(size >= (24 if cmd == 0x2C else 20), 'Short encryption command')
                cryptoff, cryptsize, cryptid = self.unpack('<3I', off+8)
                self.bounds(cryptoff, cryptsize)
                self.encryption.append({'offset': cryptoff, 'size': cryptsize, 'cryptid': cryptid})
            elif cmd in (0x22, 0x80000022):
                require(size >= 48, 'Short dyld info command')
                v = self.unpack('<10I', off+8)
                self.dyld_info = {}
                for i, name in enumerate(('rebase', 'bind', 'weak_bind', 'lazy_bind', 'export')):
                    self.bounds(v[i*2], v[i*2+1])
                    self.dyld_info[name] = {'offset': v[i*2], 'size': v[i*2+1]}
            elif cmd == 0x25:
                require(size >= 16, 'Short iOS minimum version command')
                minimum, sdk = self.unpack('<2I', off+8)
                self.ios_version = {'minimum': version(minimum), 'sdk': version(sdk)}
            elif cmd == 0x80000028:
                require(size >= 24, 'Short entrypoint command')
                self.entry_offset, stack_size = self.unpack('<2Q', off+8)
                self.bounds(self.entry_offset, 4)
            off += size
        require(off == end, 'Load commands do not consume declared bytes')
        if symtab:
            so, count, st, string_size = symtab
            self.bounds(so, count*16)
            self.bounds(st, string_size)
            for i in range(count):
                n, typ, sect, desc, value = self.unpack('<IBBHQ', so+i*16)
                require(n < string_size, 'Symbol name outside string table')
                self.symbols.append({'name': self.string(st+n, st+string_size),
                                     'type': typ, 'section': sect, 'description': desc,
                                     'value': value,
                                     'undefined': not (typ & 0xE0) and (typ & 0xE) == 0,
                                     'library_ordinal': desc >> 8,
                                     'weak_reference': bool(desc & 0x40)})

    def bounds(self, off, size):
        require(0 <= off <= len(self.data) and 0 <= size <= len(self.data)-off,
                f'Out-of-file range: offset={off:#x} size={size:#x}')

    def unpack(self, fmt, off):
        self.bounds(off, struct.calcsize(fmt))
        return struct.unpack_from(fmt, self.data, off)

    def string(self, off, end):
        self.bounds(off, end-off)
        zero = self.data.find(b'\0', off, end)
        require(zero >= 0, f'Unterminated string at {off:#x}')
        return self.data[off:zero].decode('utf-8', errors='replace')

    def section_strings(self, name):
        result = []
        for s in self.sections:
            if s['name'] != name:
                continue
            self.bounds(s['offset'], s['size'])
            raw = self.data[s['offset']:s['offset']+s['size']]
            pos = 0
            for item in raw.split(b'\0'):
                if item:
                    result.append({'offset': s['offset']+pos,
                                   'address': s['address']+pos,
                                   'text': item.decode('utf-8', errors='replace')})
                pos += len(item)+1
        return result


PLIST_KEYS = ('CFBundleIdentifier', 'CFBundleExecutable', 'CFBundleName',
              'CFBundleDisplayName', 'CFBundleShortVersionString', 'CFBundleVersion',
              'MinimumOSVersion', 'DTPlatformName', 'DTSDKName', 'DTXcode',
              'UIDeviceFamily', 'UIRequiredDeviceCapabilities', 'UIBackgroundModes',
              'GCSupportedGameControllers', 'GCSupportsControllerUserInteraction')


def inspect(path, out):
    require(not out.exists(), 'Output directory already exists; choose a new evidence path')
    ipa_hash = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024*1024), b''):
            ipa_hash.update(block)
    with zipfile.ZipFile(path) as z:
        entries = z.infolist()
        require(len(entries) <= 100000, 'ZIP entry limit exceeded')
        names = [e.filename for e in entries]
        require(len(names) == len(set(names)), 'Duplicate ZIP names')
        require(sum(e.file_size for e in entries) <= 2*1024**3, 'ZIP size limit exceeded')
        require(all(e.file_size <= 128*1024**2 for e in entries), 'ZIP member limit exceeded')
        require(not any(e.flag_bits & 1 for e in entries), 'ZIP-level encryption unsupported')
        main = [n for n in names if re.fullmatch(r'Payload/[^/]+\.app/Info\.plist', n)]
        require(len(main) == 1, 'Expected one top-level app Info.plist')
        info = plistlib.loads(z.read(main[0]))
        exe_name = info['CFBundleExecutable']
        require(isinstance(exe_name, str) and exe_name not in ('', '.', '..')
                and '/' not in exe_name and '\\' not in exe_name, 'Invalid CFBundleExecutable')
        exe_path = main[0].rsplit('/', 1)[0] + '/' + exe_name
        binary = z.read(exe_path)
        mach = MachO(binary)
        require(not any(e['cryptid'] for e in mach.encryption),
                'Executable declares encrypted code; code inspection halted')
        inventory = []
        pngs = []
        extra_macho = []
        for e in entries:
            n = e.filename
            inventory.append({'name': n, 'size': e.file_size, 'compressed_size': e.compress_size})
            if e.is_dir():
                continue
            with z.open(e) as f:
                head = f.read(40)
            if head[:4] in (b'\xcf\xfa\xed\xfe', b'\xce\xfa\xed\xfe',
                            b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'):
                extra_macho.append(n)
            if n.lower().endswith('.png'):
                require(head[:8] == b'\x89PNG\r\n\x1a\n', f'Invalid PNG signature: {n}')
                # Only standard IHDR-first PNGs are decoded here. CgBI is inventoried separately.
                if head[12:16] == b'IHDR':
                    w, h, depth, color = struct.unpack_from('>IIBB', head, 16)
                    pngs.append({'name': n, 'width': w, 'height': h,
                                 'bit_depth': depth, 'color_type': color})
                else:
                    pngs.append({'name': n, 'first_chunk': head[12:16].decode(errors='replace')})
        undefined = [s for s in mach.symbols if s['undefined']]
        imports_by_library = Counter()
        for s in undefined:
            ordinal = s['library_ordinal']
            lib = mach.libraries[ordinal-1]['path'] if 1 <= ordinal <= len(mach.libraries) else f'special:{ordinal}'
            s['library'] = lib
            imports_by_library[lib] += 1
        selectors = mach.section_strings('__objc_methname')
        classes = mach.section_strings('__objc_classname')
        cstrings = mach.section_strings('__cstring')
        terms = re.compile(r'cocos|Cocos|CocosDenshion|OpenGL|OpenAL|GL_|Metal|shader|Shader|engine|Engine')
        marked = [s for s in cstrings if terms.search(s['text'])]
        summary = {
            'scope': 'STATIC_ONLY; no guest code executed; no device validation',
            'ipa': {'filename': path.name, 'bytes': path.stat().st_size,
                    'sha256': ipa_hash.hexdigest(), 'entry_count': len(entries),
                    'uncompressed_bytes': sum(e.file_size for e in entries)},
            'plist': {k: info[k] for k in PLIST_KEYS if k in info},
            'executable': {'zip_member': exe_path, 'bytes': len(binary), 'sha256': sha(binary)},
            'header': mach.header, 'segments': mach.segments, 'sections': mach.sections,
            'commands': mach.commands, 'libraries': mach.libraries,
            'encryption_commands': mach.encryption, 'dyld_info': mach.dyld_info,
            'mach_o_ios_version': mach.ios_version, 'entry_file_offset': mach.entry_offset,
            'symbol_count': len(mach.symbols), 'undefined_symbol_count': len(undefined),
            'imports_by_library': dict(sorted(imports_by_library.items())),
            'objc_method_name_strings': len(selectors),
            'objc_class_name_strings': len(classes),
            'objc_classlist_entries': sum(s['size']//8 for s in mach.sections if s['name']=='__objc_classlist'),
            'mach_o_members': extra_macho,
            'extension_counts': dict(Counter(PurePosixPath(n).suffix.lower() or '(none)'
                                             for n in names if not n.endswith('/'))),
            'png_count': len(pngs),
            'png_dimensions_max': [max((p.get('width', 0) for p in pngs), default=0),
                                   max((p.get('height', 0) for p in pngs), default=0)],
            'png_rgba8_all_assets_bytes_hypothetical': sum(p.get('width', 0)*p.get('height', 0)*4 for p in pngs),
            'notes': ['All-assets RGBA8 sum is NOT measured resident memory.',
                      'cryptid=0 is a declaration; disassembly is a separate check.',
                      'Info.plist versions are metadata and may differ from Mach-O deployment target.',
                      'No provisioning profiles, signing identities or receipt contents exported.'],
        }
        out.mkdir(parents=True, mode=0o700)
        exports = {'inventory.json': inventory, 'triage.json': summary,
                   'imports.json': undefined, 'selectors.json': selectors,
                   'class-name-strings.json': classes, 'engine-graphics-strings.json': marked,
                   'png-inventory.json': pngs}
        for name, value in exports.items():
            (out/name).write_text(json.dumps(value, indent=2, ensure_ascii=False)+'\n')
        (out/'goblin-sword.macho').write_bytes(binary)
        print(json.dumps({k: summary[k] for k in ('scope', 'ipa', 'plist', 'executable',
                         'mach_o_ios_version', 'undefined_symbol_count', 'objc_classlist_entries',
                         'png_count', 'png_dimensions_max')}, indent=2, ensure_ascii=False))
        print('Evidence:', out)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('ipa', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    inspect(args.ipa, args.output)
