#!/bin/bash
# assemble_runtime.sh -- monta o runtime_root V5 do port (o que o nxbootstrap
# pina em nxport.generation_runtime) e reescreve essa lista no nxproject.json.
#
# O runtime_root e' a PROPRIA pasta do port: o ELF publico, o adapter-env.sh
# selado e a arvore canonica do NXExtract copiada do framework. O port-env.sh
# editavel nasce separadamente do owner seed. Dado de jogo NUNCA entra aqui --
# assets/ e libs/ nascem da extracao do APK do dono, no aparelho.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
SRC="$REPO/suportando_outros_devices/extrator-universal"

cd "$HERE"
mkdir -p nxextract
install -m 0644 "$SRC/nxextract.py"                nxextract/nxextract.py
install -m 0644 "$SRC/run-extractor.sh"            nxextract/run-extractor.sh
install -m 0644 "$SRC/nxextract-runtime-env.sh"    nxextract/nxextract-runtime-env.sh
install -m 0755 "$SRC/ui/release/aarch64/nxextract-ui" nxextract/nxextract-ui
chmod 0644 adapter-env.sh extractor.json
chmod 0755 blossomtales-nextos

python3 - "$HERE" <<'PY'
import hashlib, json, sys, collections, pathlib
root = pathlib.Path(sys.argv[1])
# ordem canonica de papeis exigida pelo nxbootstrap
members = [
    ("executable",            "blossomtales-nextos",                "0755"),
    ("runtime-hook",          "adapter-env.sh",                     "0644"),
    ("nxextract-recipe",      "extractor.json",                     "0644"),
    ("nxextract-engine",      "nxextract/nxextract.py",             "0644"),
    ("nxextract-runner",      "nxextract/run-extractor.sh",         "0644"),
    ("nxextract-runtime-env", "nxextract/nxextract-runtime-env.sh", "0644"),
    ("nxextract-ui",          "nxextract/nxextract-ui",             "0755"),
]
runtime = []
for role, rel, mode in members:
    data = (root / rel).read_bytes()
    runtime.append(collections.OrderedDict((
        ("mode", mode), ("path", rel), ("role", role),
        ("sha256", hashlib.sha256(data).hexdigest()),
    )))

proj = json.loads((root / "nxproject.json").read_text(encoding="utf-8"))
proj["nxport"]["generation_runtime"] = runtime

# package_payload tambem e' pinado por hash: reconferir aqui evita descobrir a
# divergencia so na geracao, depois de editar um .md.
payload = []
for rec in sorted(proj.get("package_payload", []), key=lambda r: r["path"]):
    data = (root / rec["path"]).read_bytes()
    rec = dict(rec)
    rec["sha256"] = hashlib.sha256(data).hexdigest()
    payload.append(collections.OrderedDict(sorted(rec.items())))
if payload:
    proj["package_payload"] = payload
req = set(proj["nxport"].get("required_files", []))
req.update(["blossomtales-nextos", "adapter-env.sh"])
proj["nxport"]["required_files"] = sorted(req)

def srt(o):
    if isinstance(o, dict):
        return collections.OrderedDict((k, srt(o[k])) for k in sorted(o))
    if isinstance(o, list):
        return [srt(v) for v in o]
    return o

(root / "nxproject.json").write_text(
    json.dumps(srt(proj), indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
print("generation_runtime: %d membros pinados" % len(runtime))
PY
