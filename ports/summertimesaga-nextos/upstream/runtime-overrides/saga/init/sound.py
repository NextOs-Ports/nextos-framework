import os

from saga.init import manifest
import saga.sound as sound


def _manifest_paths():
    found = False

    for path in manifest:
        found = True
        yield path

    if found:
        return

    root = os.environ.get("SUMMERTIME_ASSETS", "./assets")
    index = os.path.join(root, "x-game", ".summertime-index.txt")

    try:
        with open(index, "r", encoding="utf-8") as stream:
            for line in stream:
                path = line.strip()
                if path:
                    yield path
    except OSError:
        return


def _drop_x(value):
    if value.startswith("x-"):
        return value[2:]
    return value


def _normalise_sound_path(path):
    if path.startswith("x-sound/"):
        parts = [_drop_x(item) for item in path.split("/")]
        if len(parts) == 2:
            parts.insert(1, "sfx")
        return "/".join(parts)

    if path.startswith("sound/"):
        rest = path[len("sound/") :]
        if "/" not in rest:
            return "sound/sfx/" + rest

    return path


sound_manifest = []
seen = set()

for path in _manifest_paths():
    normalised = _normalise_sound_path(path)
    if not normalised.startswith("sound/"):
        continue
    if normalised in seen:
        continue
    seen.add(normalised)
    sound_manifest.append(normalised)


def _seed(cache, path):
    name = os.path.basename(path)
    if not name.endswith(".ogg"):
        return

    stem = name[:-4]
    bits = stem.split(";")
    key = bits[0]
    tags = bits[1:]

    cache.setdefault(key, []).append((tags, path))


for path in sound_manifest:
    if path.startswith("sound/music/"):
        _seed(sound.music.cache, path)
    elif path.startswith("sound/sfx/"):
        _seed(sound.sfx.cache, path)

if os.environ.get("SUMMERTIME_LOG"):
    print("Summertime sound manifest count:", len(sound_manifest))
    print("Summertime sound manifest sample:", sound_manifest[-12:])
    print(
        "Summertime music cache keys:",
        sorted(getattr(sound.music, "cache", {}).keys()),
    )
    print(
        "Summertime sfx cache keys:",
        sorted(getattr(sound.sfx, "cache", {}).keys()),
    )

sound.finalise()
