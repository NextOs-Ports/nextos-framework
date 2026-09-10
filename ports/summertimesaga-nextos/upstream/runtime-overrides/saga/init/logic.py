import importlib
import os
import pkgutil

from renpy.exports import load_module

import saga.logic


def _module_names():
    seen = set()

    try:
        from saga.init import manifest
    except Exception:
        manifest = ()

    for path in manifest:
        if not path.startswith("saga/logic/"):
            continue
        stem = path.partition(".")[0]
        name = stem.replace("/", ".")
        if name.endswith(".__init__") or name in seen:
            continue
        seen.add(name)
        yield name

    for info in pkgutil.iter_modules(saga.logic.__path__):
        if info.name == "__init__":
            continue
        name = "saga.logic." + info.name
        if name in seen:
            continue
        seen.add(name)
        yield name


_count = 0
for _name in sorted(_module_names()):
    importlib.import_module(_name)
    _count += 1

if os.environ.get("SUMMERTIME_LOG"):
    print("Summertime logic modules imported:", _count)

load_module("res/meta/step")

_event_module = importlib.import_module("saga.event")
from saga.logic.auto import cancel, pump

_annotate = getattr(_event_module, "annotate", None) or getattr(
    _event_module.impl, "annotate", None
)
if _annotate is not None:
    cancel = _annotate(cancel)
    pump = _annotate(pump)

_event_module.impl.auto_cancel = cancel
_event_module.impl.auto_pump = pump
