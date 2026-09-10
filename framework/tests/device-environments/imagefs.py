#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Leitura somente leitura de imagens oficiais de firmware, sem montar nada.

Este módulo é a parte comum dos ambientes locais por device. Ele existe para
que cada ambiente novo descreva o SEU aparelho no contrato, em vez de repetir
o mesmo código de partição, ext4, FAT e ELF.

Fronteiras que valem para todos os ambientes:

- a imagem oficial nunca entra no Git e nunca é redistribuída;
- a leitura é somente leitura: `debugfs` para ext4 e `mcopy` para FAT, sempre
  com deslocamento em bytes da partição, sem montar, sem loop device, sem root
  e sem escrever um byte no arquivo de origem;
- nada aqui inicializa vídeo, áudio ou input, e nada toca DRM/Mali/framebuffer.
"""

import hashlib
import os
import posixpath
import shutil
import struct
import subprocess
from pathlib import Path, PurePosixPath

CHUNK = 1 << 22
# Nenhuma leitura de imagem pode demorar indefinidamente num gate.
COMMAND_TIMEOUT = 60
REPOSITORY = Path(__file__).resolve().parents[3]
UNSAFE_MARKS = ('"', "\x00", "\n", "\r")
TRUSTED_TOOL_PATH = os.defpath


class ImageError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise ImageError(message)


def outside_repository(path, label):
    """Falha fechado depois de resolver os pais existentes e os symlinks."""
    resolved = Path(path).resolve(strict=False)
    try:
        resolved.relative_to(REPOSITORY)
    except ValueError:
        return resolved
    raise ImageError("%s must stay outside the repository: %s"
                     % (label, resolved))


def safe_argument(value, label):
    """Caminho de linha de comando: absoluto, sem symlink e sem marca suja."""
    path = Path(value)
    require(path.is_absolute(), "%s must be an absolute path" % label)
    require(not any(mark in str(path) for mark in UNSAFE_MARKS),
            "%s contains unsafe characters" % label)
    require(not path.is_symlink(), "%s is a symlink" % label)
    return path


def safe_logical_path(root, logical):
    """Converte um caminho da imagem num caminho contido no ambiente."""
    require(isinstance(logical, str) and logical.startswith("/") and
            not logical.startswith("//") and
            not any(mark in logical for mark in UNSAFE_MARKS),
            "unsafe image path: %r" % logical)
    normalized = posixpath.normpath(logical)
    require(normalized == logical and ".." not in PurePosixPath(logical).parts,
            "non-canonical image path: %r" % logical)
    local = root.joinpath(*PurePosixPath(logical).parts[1:])
    resolved_root = root.resolve(strict=False)
    resolved = local.resolve(strict=False)
    try:
        resolved.relative_to(resolved_root)
    except ValueError:
        raise ImageError("image path escaped the prepared root: %s" % logical)
    return local


def environment_argument(value, label="--environment"):
    """Ambiente preparado: absoluto, não-symlink e fora do repositório.

    A checagem de symlink vem ANTES de resolver: um link plantado no lugar do
    ambiente resolveria para outro diretório e o gate leria a árvore errada
    achando que leu a certa.
    """
    path = safe_argument(value, label)
    require(path.exists(), "%s does not exist: %s" % (label, path))
    resolved = outside_repository(path.resolve(strict=True), label)
    require(resolved.is_dir(), "%s is not a directory: %s" % (label, resolved))
    return resolved


def fixed_layout(receipt, key, expected):
    """Aceita só o layout que o ambiente declara no código, não no recibo.

    O recibo é dado de entrada; deixá-lo escolher o subdiretório permitiria
    apontar a verificação para qualquer lugar.
    """
    layout = receipt.get("layout")
    require(isinstance(layout, dict), "receipt has no layout")
    require(layout.get(key) == expected,
            "receipt layout %r is %r, this environment only reads %r"
            % (key, layout.get(key), expected))
    return expected


def resolve_tool(name, required=True):
    """Resolve uma ferramenta uma vez e devolve somente caminho absoluto.

    Os chamadores guardam este valor e o usam diretamente no ``argv``. Assim,
    o ``PATH`` mínimo entregue ao subprocesso não pode trocar o executável
    entre a descoberta e a execução.
    """
    require(isinstance(name, str) and name and Path(name).name == name,
            "unsafe tool name: %r" % name)
    found = shutil.which(name, path=TRUSTED_TOOL_PATH)
    if found is None:
        if required:
            raise ImageError("%s is not available" % name)
        return None
    try:
        resolved = Path(found).resolve(strict=True)
    except OSError as error:
        raise ImageError("%s cannot be resolved: %s" % (name, error))
    require(resolved.is_absolute() and resolved.is_file()
            and os.access(resolved, os.X_OK),
            "%s did not resolve to an executable file: %s" %
            (name, resolved))
    return str(resolved)


def require_absolute_executable(command, tool):
    require(isinstance(command, (list, tuple)) and command,
            "%s command is empty" % tool)
    try:
        executable = Path(os.fspath(command[0]))
    except TypeError:
        raise ImageError("%s executable is not a path" % tool)
    require(executable.is_absolute(),
            "%s executable must be an absolute path: %s" %
            (tool, executable))


def run_guarded(command, tool, timeout=COMMAND_TIMEOUT, cwd=None):
    """Executa uma ferramenta externa sem herdar ambiente e com teto de tempo."""
    require_absolute_executable(command, tool)
    try:
        return subprocess.run(command, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, check=False,
                              env=clean_subprocess_env(),
                              timeout=timeout,
                              cwd=str(cwd) if cwd else None)
    except subprocess.TimeoutExpired:
        raise ImageError("%s timed out" % tool)
    except FileNotFoundError:
        raise ImageError("%s is not available" % tool)


def clean_subprocess_env():
    """Ambiente mínimo: nada herdado decide o que a ferramenta vai ler."""
    return {
        "PATH": TRUSTED_TOOL_PATH,
        "LANG": "C",
        "LC_ALL": "C",
    }


def run_tool(command, tool):
    require_absolute_executable(command, tool)
    try:
        return subprocess.run(command, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, check=False,
                              env=clean_subprocess_env(),
                              timeout=COMMAND_TIMEOUT)
    except subprocess.TimeoutExpired:
        raise ImageError("%s timed out while reading the image" % tool)


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(CHUNK), b""):
            digest.update(chunk)
    return digest.hexdigest()


def partitions(image):
    """Lista as partições da imagem, GPT ou MBR, sem montar."""
    image = Path(image)
    with image.open("rb") as stream:
        mbr = stream.read(512)
        require(len(mbr) == 512 and mbr[510:512] == b"\x55\xaa",
                "image has no partition table")
        entries = []
        gpt = any(mbr[446 + 16 * index + 4] == 0xEE for index in range(4))
        if gpt:
            stream.seek(512)
            header = stream.read(92)
            require(header[:8] == b"EFI PART", "GPT header is missing")
            entry_lba = struct.unpack("<Q", header[72:80])[0]
            count = struct.unpack("<I", header[80:84])[0]
            size = struct.unpack("<I", header[84:88])[0]
            require(0 < count <= 256 and 128 <= size <= 4096,
                    "GPT partition array is implausible")
            stream.seek(entry_lba * 512)
            table = stream.read(count * size)
            for index in range(count):
                entry = table[index * size:(index + 1) * size]
                if entry[:16] == b"\x00" * 16:
                    continue
                first = struct.unpack("<Q", entry[32:40])[0]
                last = struct.unpack("<Q", entry[40:48])[0]
                name = entry[56:128].decode("utf-16-le").rstrip("\x00")
                entries.append({
                    "index": index + 1, "name": name, "type": "gpt",
                    "offset": first * 512, "size": (last - first + 1) * 512,
                })
        else:
            for index in range(4):
                entry = mbr[446 + 16 * index:446 + 16 * (index + 1)]
                kind = entry[4]
                if not kind:
                    continue
                start = struct.unpack("<I", entry[8:12])[0]
                sectors = struct.unpack("<I", entry[12:16])[0]
                entries.append({
                    "index": index + 1, "name": "", "type": "mbr",
                    "mbr_type": kind, "offset": start * 512,
                    "size": sectors * 512,
                })
    require(entries, "no partition found in the image")
    return entries


def partition_at(image, offset):
    for entry in partitions(image):
        if entry["offset"] == offset:
            return entry
    raise ImageError("no partition starts at offset %d" % offset)


def partition_named(image, name):
    for entry in partitions(image):
        if entry.get("name") == name:
            return entry
    raise ImageError("partition %r not found" % name)


def filesystem_kind(image, offset):
    """Identifica o sistema de arquivos pela assinatura, sem montar."""
    with Path(image).open("rb") as stream:
        stream.seek(offset + 1024)
        superblock = stream.read(120)
        if len(superblock) >= 58 and superblock[56:58] == b"\x53\xef":
            return "ext"
        stream.seek(offset)
        boot = stream.read(512)
    if len(boot) >= 512:
        if boot[54:59] == b"FAT12" or boot[54:59] == b"FAT16":
            return "fat"
        if boot[82:87] == b"FAT32":
            return "fat"
        if boot[3:11] == b"EXFAT   ":
            return "exfat"
    with Path(image).open("rb") as stream:
        stream.seek(offset)
        magic = stream.read(4)
    if magic == b"hsqs":
        return "squashfs"
    return "unknown"


class ExtReader:
    """Leitor ext2/3/4 baseado em `debugfs`, somente leitura."""

    kind = "ext"

    def __init__(self, image, offset):
        self.debugfs = resolve_tool("debugfs")
        self.spec = "%s?offset=%d" % (image, offset)

    def _run(self, command):
        return run_tool([self.debugfs, "-R", command, self.spec], "debugfs")

    @staticmethod
    def _check(path):
        require(isinstance(path, str) and path.startswith("/") and
                not path.startswith("//") and
                not any(mark in path for mark in UNSAFE_MARKS),
                "unsafe image path: %r" % path)
        require(posixpath.normpath(path) == path and
                ".." not in PurePosixPath(path).parts,
                "non-canonical image path: %r" % path)
        return path

    def exists(self, path):
        self._check(path)
        result = self._run('stat "%s"' % path)
        return result.returncode == 0 and b"Inode:" in result.stdout

    def is_symlink(self, path):
        self._check(path)
        result = self._run('stat "%s"' % path)
        return b"Type: symlink" in result.stdout

    def listdir(self, path):
        self._check(path)
        result = self._run('ls -p "%s"' % path)
        if result.returncode != 0:
            return []
        # Formato do `ls -p`: /inode/modo/uid/gid/nome/tamanho/
        names = []
        for line in result.stdout.decode("utf-8", "replace").splitlines():
            fields = line.strip().split("/")
            if len(fields) > 5 and fields[5] not in (".", ".."):
                names.append(fields[5])
        return names

    def resolve(self, path):
        self._check(path)
        seen = set()
        current = path
        for _ in range(16):
            result = self._run('stat "%s"' % current)
            require(result.returncode == 0, "missing in the image: %s" % path)
            text = result.stdout.decode("utf-8", "replace")
            if "Type: symlink" not in text:
                return current
            target = text.split("Fast link dest: ")[-1].split("\n")[0]
            target = target.strip().strip('"')
            require(target, "unreadable symlink: %s" % current)
            current = posixpath.normpath(
                target if target.startswith("/") else
                posixpath.join(posixpath.dirname(current), target))
            require(current.startswith("/") and not current.startswith("//"),
                    "symlink escaped the image root: %s" % path)
            self._check(current)
            require(current not in seen, "symlink loop at %s" % path)
            seen.add(current)
        raise ImageError("symlink chain too deep: %s" % path)

    def read(self, path, destination):
        self._check(path)
        require(not destination.exists() and not destination.is_symlink(),
                "destination already exists: %s" % destination)
        destination.parent.mkdir(parents=True, exist_ok=True)
        # O parser do debugfs separa por espaço: os dois caminhos vão entre
        # aspas para que um destino com espaço continue válido.
        result = self._run('dump -p "%s" "%s"' % (path, destination))
        require(result.returncode == 0 and destination.is_file()
                and not destination.is_symlink(),
                "cannot read %s: %s"
                % (path, result.stderr.decode("utf-8", "replace").strip()))


class FatReader:
    """Leitor FAT baseado em `mcopy`/`mdir` das mtools, somente leitura."""

    kind = "fat"

    def __init__(self, image, offset):
        self.tools = {name: resolve_tool(name)
                      for name in ("mcopy", "mdir", "mtype")}
        self.image = str(image)
        self.offset = offset

    def _env(self):
        environment = clean_subprocess_env()
        environment["MTOOLS_SKIP_CHECK"] = "1"
        return environment

    def _drive(self, path):
        require(isinstance(path, str) and path.startswith("/") and
                not any(mark in path for mark in UNSAFE_MARKS),
                "unsafe image path: %r" % path)
        require(posixpath.normpath(path) == path and
                ".." not in PurePosixPath(path).parts,
                "non-canonical image path: %r" % path)
        return "::%s" % path.lstrip("/")

    def _run(self, tool, *arguments):
        executable = self.tools[tool]
        try:
            return subprocess.run(
                [executable, "-i", "%s@@%d" % (self.image, self.offset)] +
                list(arguments), stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, check=False, env=self._env(),
                timeout=COMMAND_TIMEOUT)
        except subprocess.TimeoutExpired:
            raise ImageError("%s timed out while reading the image" % tool)

    def exists(self, path):
        return self._run("mdir", "-b", self._drive(path)).returncode == 0 or \
            self._run("mtype", self._drive(path)).returncode == 0

    def is_symlink(self, path):
        return False

    def listdir(self, path):
        result = self._run("mdir", "-b", self._drive(path))
        if result.returncode != 0:
            return []
        names = []
        for line in result.stdout.decode("utf-8", "replace").splitlines():
            name = line.strip().rstrip("/").split("/")[-1]
            if name and name not in (".", ".."):
                names.append(name)
        return names

    def resolve(self, path):
        return path

    def read(self, path, destination):
        require(not destination.exists() and not destination.is_symlink(),
                "destination already exists: %s" % destination)
        destination.parent.mkdir(parents=True, exist_ok=True)
        result = self._run("mcopy", "-n", self._drive(path), str(destination))
        require(result.returncode == 0 and destination.is_file()
                and not destination.is_symlink(),
                "cannot read %s: %s"
                % (path, result.stderr.decode("utf-8", "replace").strip()))


class ZipReader:
    """Leitor somente leitura de uma distribuição em ZIP (árvore de cartão).

    Algumas CFWs não são distribuídas como imagem de disco, e sim como o
    conteúdo do cartão. O contrato do ambiente continua o mesmo: arquivo
    oficial verificado por tamanho e SHA-256, leitura somente leitura e nada
    versionado no Git.
    """

    kind = "zip"

    def __init__(self, archive):
        import zipfile
        self._zip = zipfile.ZipFile(str(archive), "r")
        self._names = set()
        for info in self._zip.infolist():
            name = info.filename
            require(not name.startswith("/"),
                    "absolute member in the archive: %r" % name)
            require(".." not in PurePosixPath(name).parts,
                    "member escapes the archive root: %r" % name)
            require(not any(mark in name for mark in ("\x00", "\n", "\r")),
                    "unsafe member name: %r" % name)
            require(name not in self._names,
                    "duplicated member in the archive: %r" % name)
            # Modo alto do ZIP: S_IFLNK (0xA000) é entrada de symlink.
            mode = info.external_attr >> 16
            require(not (info.create_system == 3 and mode & 0xF000 == 0xA000),
                    "symlink member in the archive: %r" % name)
            self._names.add(name)

    def _member(self, path):
        require(isinstance(path, str) and path.startswith("/") and
                not any(mark in path for mark in UNSAFE_MARKS),
                "unsafe archive path: %r" % path)
        require(posixpath.normpath(path) == path and
                ".." not in PurePosixPath(path).parts,
                "non-canonical archive path: %r" % path)
        return path.lstrip("/")

    def exists(self, path):
        name = self._member(path)
        return name in self._names or (name + "/") in self._names

    def is_symlink(self, path):
        return False

    def listdir(self, path):
        prefix = self._member(path).rstrip("/")
        prefix = prefix + "/" if prefix else ""
        names = set()
        for name in self._names:
            if not name.startswith(prefix) or name == prefix:
                continue
            rest = name[len(prefix):].strip("/")
            if rest:
                names.add(rest.split("/")[0])
        return sorted(names)

    def resolve(self, path):
        self._member(path)
        return path

    def header(self, path, size):
        name = self._member(path)
        require(name in self._names, "member not in the archive: %s" % path)
        with self._zip.open(name) as source:
            return source.read(size)

    def read(self, path, destination):
        name = self._member(path)
        require(name in self._names, "member not in the archive: %s" % path)
        require(not destination.exists() and not destination.is_symlink(),
                "destination already exists: %s" % destination)
        destination.parent.mkdir(parents=True, exist_ok=True)
        with self._zip.open(name) as source, destination.open("wb") as target:
            shutil.copyfileobj(source, target)
        require(destination.is_file() and not destination.is_symlink(),
                "member did not become a regular file: %s" % path)


def reader_for(image, offset):
    kind = filesystem_kind(image, offset)
    if kind == "ext":
        return ExtReader(image, offset)
    if kind == "fat":
        return FatReader(image, offset)
    raise ImageError("unsupported filesystem at offset %d: %s" % (offset, kind))


def elf_identity(path):
    with Path(path).open("rb") as stream:
        header = stream.read(20)
    if len(header) < 20 or header[:4] != b"\x7fELF":
        return None
    return header[4], struct.unpack_from("<H", header, 18)[0]


def is_gnu_ld_script(path):
    """Reconhece o script de linker que acompanha a libc em multiarch."""
    with Path(path).open("rb") as stream:
        head = stream.read(256)
    if head[:4] == b"\x7fELF":
        return False
    text = head.decode("ascii", "replace")
    return "GNU ld script" in text or "OUTPUT_FORMAT(" in text


def elf_dynamic(path):
    """DT_NEEDED e DT_SONAME de um ELF, sem depender de biblioteca externa."""
    data = Path(path).read_bytes()
    if len(data) < 64 or data[:4] != b"\x7fELF":
        return [], None
    is64 = data[4] == 2
    if is64:
        phoff = struct.unpack_from("<Q", data, 32)[0]
        phentsize = struct.unpack_from("<H", data, 54)[0]
        phnum = struct.unpack_from("<H", data, 56)[0]
    else:
        phoff = struct.unpack_from("<I", data, 28)[0]
        phentsize = struct.unpack_from("<H", data, 42)[0]
        phnum = struct.unpack_from("<H", data, 44)[0]
    dynamic = None
    for index in range(phnum):
        base = phoff + index * phentsize
        if base + phentsize > len(data):
            return [], None
        if struct.unpack_from("<I", data, base)[0] != 2:  # PT_DYNAMIC
            continue
        if is64:
            dynamic = (struct.unpack_from("<Q", data, base + 8)[0],
                       struct.unpack_from("<Q", data, base + 32)[0])
        else:
            dynamic = (struct.unpack_from("<I", data, base + 4)[0],
                       struct.unpack_from("<I", data, base + 16)[0])
        break
    if dynamic is None:
        return [], None
    offset, size = dynamic
    step = 16 if is64 else 8
    needed_offsets = []
    soname_offset = None
    strtab = None
    for position in range(offset, min(offset + size, len(data)), step):
        if is64:
            tag = struct.unpack_from("<q", data, position)[0]
            value = struct.unpack_from("<Q", data, position + 8)[0]
        else:
            tag = struct.unpack_from("<i", data, position)[0]
            value = struct.unpack_from("<I", data, position + 4)[0]
        if tag == 0:
            break
        if tag == 1:
            needed_offsets.append(value)
        elif tag == 5:
            strtab = value
        elif tag == 14:
            soname_offset = value
    if strtab is None:
        return [], None
    file_offset = None
    for index in range(phnum):
        base = phoff + index * phentsize
        if struct.unpack_from("<I", data, base)[0] != 1:  # PT_LOAD
            continue
        if is64:
            p_offset = struct.unpack_from("<Q", data, base + 8)[0]
            p_vaddr = struct.unpack_from("<Q", data, base + 16)[0]
            p_filesz = struct.unpack_from("<Q", data, base + 32)[0]
        else:
            p_offset = struct.unpack_from("<I", data, base + 4)[0]
            p_vaddr = struct.unpack_from("<I", data, base + 8)[0]
            p_filesz = struct.unpack_from("<I", data, base + 16)[0]
        if p_vaddr <= strtab < p_vaddr + p_filesz:
            file_offset = strtab - p_vaddr + p_offset
            break
    if file_offset is None:
        return [], None

    def read_string(entry):
        start = file_offset + entry
        end = data.find(b"\x00", start)
        if start >= len(data) or end <= start:
            return None
        return data[start:end].decode("ascii", "replace")

    names = [name for name in (read_string(entry) for entry in needed_offsets)
             if name]
    soname = read_string(soname_offset) if soname_offset is not None else None
    return names, soname


# Um closure de biblioteca real cabe com folga aqui; o teto existe para que um
# erro de resolução vire falha e não um despejo infinito.
MAX_LIBRARIES = 512
ARCH_ELF = {"aarch64": (2, 183), "armv7": (1, 40),
            "x86_64": (2, 62), "i386": (1, 3)}


def safe_soname(value, path):
    require("/" not in value and value not in ("", ".", "..") and
            not any(mark in value for mark in UNSAFE_MARKS),
            "unsafe DT_SONAME %r in %s" % (value, path))
    return value


def file_record(local, logical, abi=None):
    """Recibo forte de um arquivo preparado: bytes, ABI e dinâmica do ELF."""
    require(local.is_file() and not local.is_symlink(),
            "prepared path is not a regular file: %s" % logical)
    identity = elf_identity(local)
    if abi is not None and identity is not None:
        require(identity == ARCH_ELF[abi],
                "%s has the wrong ELF identity for %s: %r"
                % (logical, abi, identity))
    needed, soname = elf_dynamic(local)
    if soname is not None:
        safe_soname(soname, logical)
    return {
        "path": logical,
        "abi": abi,
        "size": local.stat().st_size,
        "sha256": sha256_file(local),
        "elf": ({"class": identity[0], "machine": identity[1]}
                if identity else None),
        "soname": soname,
        "needed": sorted(needed),
        "aliases": [],
    }


def extract_files(reader, paths, root, abi=None):
    """Extrai caminhos declarados, contidos no ambiente, com recibo por arquivo."""
    records = []
    seen = set()
    for logical in paths:
        require(logical not in seen, "duplicated declared path: %s" % logical)
        seen.add(logical)
        destination = safe_logical_path(root, logical)
        require(not destination.exists() and not destination.is_symlink(),
                "duplicate prepared path: %s" % logical)
        reader.read(reader.resolve(logical), destination)
        records.append(file_record(destination, logical, abi))
    return records


def extract_closure(reader, seeds, search_directories, root, abi=None):
    """Extrai as sementes e o fechamento DT_NEEDED delas dentro das raízes.

    Devolve (registros, apelidos). Um `DT_NEEDED` que não resolve dentro das
    raízes daquela ABI é FALHA: um closure incompleto é uma promessa que o
    aparelho não cumpre. Os apelidos de SONAME são criados apenas DENTRO do
    ambiente preparado -- a imagem de origem não é tocada -- porque o
    carregador procura pelo SONAME.
    """
    records = []
    aliases = []
    pending = list(seeds)
    done = set()
    while pending:
        require(len(done) < MAX_LIBRARIES,
                "library closure exceeded %d files" % MAX_LIBRARIES)
        logical = pending.pop(0)
        if logical in done:
            continue
        done.add(logical)
        destination = safe_logical_path(root, logical)
        require(not destination.exists() and not destination.is_symlink(),
                "duplicate prepared path: %s" % logical)
        reader.read(reader.resolve(logical), destination)
        record = file_record(destination, logical, abi)
        require(record["size"] > 0, "%s came out empty" % logical)
        records.append(record)
        for name in record["needed"]:
            safe_soname(name, logical)
            resolved = False
            for directory in search_directories:
                candidate = posixpath.join(directory, name)
                if candidate in done:
                    resolved = True
                    break
                if reader.exists(candidate):
                    pending.append(candidate)
                    resolved = True
                    break
            require(resolved,
                    "unresolved DT_NEEDED %s required by %s" % (name, logical))

    by_path = {record["path"]: record for record in records}
    published = {}
    for record in records:
        soname = record["soname"]
        if not soname or soname == PurePosixPath(record["path"]).name:
            continue
        alias_path = posixpath.join(posixpath.dirname(record["path"]), soname)
        if alias_path in by_path:
            continue
        previous = published.get(alias_path)
        if previous is not None:
            require(by_path[previous]["sha256"] == record["sha256"],
                    "conflicting SONAME alias: %s" % alias_path)
            continue
        alias = safe_logical_path(root, alias_path)
        require(not alias.exists() and not alias.is_symlink(),
                "alias collides with a prepared path: %s" % alias_path)
        alias.symlink_to(PurePosixPath(record["path"]).name)
        published[alias_path] = record["path"]
        record["aliases"].append(alias_path)
        aliases.append(alias_path)

    records.sort(key=lambda record: record["path"])
    return records, sorted(aliases)


def audit_directory_abi(reader, directory, expected_abi, limit=4096):
    """Classifica a ABI de TODOS os ELFs de um diretório, sem extrair tudo.

    Existe para que o ambiente possa dizer "auditei o diretório inteiro" em vez
    de "auditei os arquivos que escolhi extrair". Lê só o cabeçalho de cada
    membro; nada é escrito.
    """
    names = reader.listdir(directory)
    require(names, "directory has no entries to audit: %s" % directory)
    require(len(names) <= limit,
            "directory is larger than the audit limit: %s" % directory)
    expected = ARCH_ELF[expected_abi]
    total = 0
    elves = 0
    foreign = []
    directories = 0
    for name in sorted(names):
        logical = posixpath.join(directory, name)
        if not reader.exists(logical):
            continue
        total += 1
        try:
            header = reader.header(logical, 20)
        except ImageError:
            # Subdiretório: contado, mas não é arquivo para classificar.
            directories += 1
            continue
        if len(header) < 20 or header[:4] != b"\x7fELF":
            continue
        elves += 1
        identity = (header[4], struct.unpack_from("<H", header, 18)[0])
        if identity != expected:
            foreign.append("%s:%s" % (name, identity))
    return {"directory": directory, "entries": total,
            "subdirectories": directories, "elves": elves,
            "expected_abi": expected_abi, "foreign": sorted(foreign)}


def verify_records(root, records, aliases=(), complete_closures=None):
    """Recusa arquivo alterado, ausente, extra ou virado symlink.

    A varredura é do ambiente inteiro: qualquer caminho que não esteja no
    recibo é sobra, e sobra invalida a prova tanto quanto byte trocado.

    ``complete_closures`` é opt-in e mapeia ABI para as raízes de busca que
    aquela closure promete fechar. Inventários seletivos devem omiti-lo.
    """
    require(root.is_dir() and not root.is_symlink(),
            "prepared root is missing or is a symlink")
    expected = {}
    records_by_path = {}
    alias_owners = {}
    for record in records:
        require(record["path"] not in records_by_path,
                "duplicated prepared record: %s" % record["path"])
        local = safe_logical_path(root, record["path"])
        require(local.is_file() and not local.is_symlink(),
                "prepared file is missing or became a symlink: %s"
                % record["path"])
        require(local.stat().st_size == record["size"],
                "prepared file changed size: %s" % record["path"])
        require(sha256_file(local) == record["sha256"],
                "prepared file changed bytes: %s" % record["path"])
        identity = elf_identity(local)
        if record.get("elf"):
            require(identity == (record["elf"]["class"],
                                 record["elf"]["machine"]),
                    "prepared ELF changed identity: %s" % record["path"])
            if record.get("abi"):
                require(identity == ARCH_ELF[record["abi"]],
                        "prepared ELF is not %s: %s"
                        % (record["abi"], record["path"]))
        else:
            require(identity is None,
                    "a non-ELF record became an ELF: %s" % record["path"])
        needed, soname = elf_dynamic(local)
        require(soname == record.get("soname"),
                "prepared DT_SONAME changed: %s" % record["path"])
        require(sorted(needed) == list(record.get("needed", [])),
                "prepared DT_NEEDED changed: %s" % record["path"])
        expected[local.resolve()] = record["path"]
        records_by_path[record["path"]] = record
        record_aliases = record.get("aliases", [])
        require(isinstance(record_aliases, list),
                "prepared record has malformed aliases: %s" % record["path"])
        for alias_path in record_aliases:
            safe_logical_path(root, alias_path)
            require(alias_path not in alias_owners,
                    "alias has more than one declared owner: %s" % alias_path)
            alias_owners[alias_path] = record["path"]

    aliases = list(aliases)
    require(len(aliases) == len(set(aliases)),
            "duplicated alias in the environment inventory")
    require(set(aliases) == set(alias_owners),
            "environment aliases differ from record aliases")
    alias_targets = {}
    for alias_path, owner_path in alias_owners.items():
        alias = safe_logical_path(root, alias_path)
        require(alias.is_symlink(), "declared alias is not a symlink: %s"
                % alias_path)
        target = alias.resolve()
        owner = safe_logical_path(root, owner_path).resolve()
        require(target == owner,
                "alias does not point at its declared owner: %s -> %s"
                % (alias_path, owner_path))
        alias_targets[alias.absolute()] = alias_path

    if complete_closures is None:
        complete_closures = {}
    require(isinstance(complete_closures, dict),
            "complete closures must map ABI names to search roots")
    for abi, search_directories in complete_closures.items():
        require(abi in ARCH_ELF, "unknown complete-closure ABI: %r" % abi)
        require(isinstance(search_directories, list) and search_directories,
                "complete closure has no search roots for %s" % abi)
        require(len(search_directories) == len(set(search_directories)),
                "complete closure repeats a search root for %s" % abi)
        for directory in search_directories:
            safe_logical_path(root, directory)

        members = {path: record for path, record in records_by_path.items()
                   if record.get("abi") == abi}
        require(members, "complete closure has no records for %s" % abi)
        available = dict(members)
        for alias_path, owner_path in alias_owners.items():
            if owner_path in members:
                available[alias_path] = members[owner_path]
        for logical, record in members.items():
            for name in record.get("needed", []):
                safe_soname(name, logical)
                resolved = any(posixpath.join(directory, name) in available
                               for directory in search_directories)
                require(resolved,
                        "unresolved DT_NEEDED %s required by %s in the %s "
                        "complete closure" % (name, logical, abi))

    for path in sorted(root.rglob("*")):
        if path.is_dir() and not path.is_symlink():
            continue
        if path.is_symlink():
            require(path.absolute() in alias_targets,
                    "undeclared symlink in the environment: %s" % path)
            continue
        require(path.resolve() in expected,
                "undeclared file in the environment: %s" % path)
    return len(expected), len(alias_targets)
