import os


_INDEX_EXTENSIONS = (
    ".rpy", ".rpyc", ".rpym", ".rpymc", ".rpyb", ".rpa", ".rpi",
    ".png", ".jpg", ".jpeg", ".webp", ".sdf",
    ".ogg", ".opus", ".mp3", ".wav", ".flac",
    ".mkv", ".webm", ".mp4",
    ".ttf", ".otf",
)


class _LazyInfo:
    def __init__(self, apk):
        self.apk = apk

    def __contains__(self, fn):
        return os.path.isfile(self.apk.path_for(fn))

    def __iter__(self):
        return iter(self.apk.list())


class SubFile:
    def __init__(self, name, base=0, length=None):
        self.name = name
        self.base = base
        self.length = length
        self._f = None

    def open(self):
        self._f = open(self.name, "rb")
        self._f.seek(self.base)
        return self

    def __enter__(self):
        return self.open()

    def __exit__(self, _type, value, tb):
        self.close()
        return False

    def read(self, length=None):
        if self._f is None:
            self.open()
        if self.length is None:
            return self._f.read(length)
        remaining = self.length - (self._f.tell() - self.base)
        if length is None or length > remaining:
            length = remaining
        return self._f.read(max(length, 0))

    def readline(self, length=None):
        if self._f is None:
            self.open()
        return self._f.readline(length if length is not None else -1)

    def readlines(self, length=None):
        if self._f is None:
            self.open()
        return self._f.readlines(length if length is not None else -1)

    def xreadlines(self):
        return self

    def __iter__(self):
        return self

    def __next__(self):
        rv = self.readline()
        if not rv:
            raise StopIteration()
        return rv

    next = __next__

    def flush(self):
        return None

    def seek(self, offset, whence=0):
        if self._f is None:
            self.open()
        if whence == 0:
            return self._f.seek(self.base + offset, 0)
        return self._f.seek(offset, whence)

    def tell(self):
        if self._f is None:
            self.open()
        return self._f.tell() - self.base

    def close(self):
        if self._f is not None:
            self._f.close()
            self._f = None


class APK:
    def __init__(self, apk=None, prefix="assets/"):
        root = os.environ.get("SUMMERTIME_ASSETS", "./assets")
        prefix = prefix or "assets/"
        if prefix.startswith("./assets/"):
            prefix = prefix[9:]
        elif prefix.startswith("assets/"):
            prefix = prefix[7:]
        self.root = os.path.join(root, prefix)
        self.info = _LazyInfo(self)
        self._files = []
        self._indexed = False

    def path_for(self, fn):
        path = os.path.join(self.root, fn)
        if os.path.isfile(path):
            return path
        if fn.startswith("sound/"):
            parts = fn.split("/")
            if len(parts) >= 3:
                parts = ["x-" + parts[0]] + ["x-" + i for i in parts[1:]]
                return os.path.join(self.root, *parts)
            if len(parts) == 2:
                return os.path.join(self.root, "x-sound", "x-sfx", "x-" + parts[1])
        if fn.startswith("x-sound/x-"):
            return os.path.join(self.root, "x-sound", "x-sfx", fn[len("x-sound/"):])
        return path

    def _index_roots(self):
        base = os.path.basename(os.path.normpath(self.root))
        if base == "x-game":
            return [
                os.path.join(self.root, "x-src"),
                os.path.join(self.root, "x-res"),
                os.path.join(self.root, "x-saga"),
                os.path.join(self.root, "x-cache"),
                os.path.join(self.root, "x-art"),
                os.path.join(self.root, "x-font"),
                os.path.join(self.root, "x-sound"),
            ]
        return [self.root]

    def _index(self):
        if self._indexed:
            return
        manifest = os.path.join(self.root, ".summertime-index.txt")
        if os.path.isfile(manifest):
            with open(manifest, "r", encoding="utf-8") as f:
                self._files.extend(i.strip() for i in f if i.strip())
            self._indexed = True
            return
        for root in self._index_roots():
            if not os.path.isdir(root):
                continue
            for base, dirs, files in os.walk(root):
                dirs[:] = [d for d in dirs if not d.startswith(".")]
                for fn in files:
                    if fn.startswith("."):
                        continue
                    full = os.path.join(base, fn)
                    rel = os.path.relpath(full, self.root).replace("\\", "/")
                    if rel.lower().endswith(_INDEX_EXTENSIONS):
                        if rel.startswith("x-sound/x-sfx/"):
                            rel = "x-sound/" + rel[len("x-sound/x-sfx/"):]
                        self._files.append(rel)
        self._indexed = True

    def list(self):
        self._index()
        return sorted(self._files)

    def open(self, fn):
        path = self.path_for(fn)
        if not os.path.isfile(path):
            raise IOError(fn)
        return open(path, "rb")
