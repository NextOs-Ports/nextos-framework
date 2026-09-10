#!/usr/bin/env python3
"""make_nxrelease.py -- monta o nxrelease.json a partir da arvore GERADA.

O manifesto tem de descrever exatamente os bytes que o nxgenerator produziu.
Escrever isso a mao convida a divergencia silenciosa entre o que foi gerado e o
que vai para o ZIP, entao ele e' derivado da propria arvore.

Uso: make_nxrelease.py <arvore-gerada> <saida.json>
"""
import hashlib, json, os, subprocess, sys, collections

PROVENANCE_BY_FILE = {
    "blossomtales-nextos":
        "compilado por build_buster.sh em Debian 10 (glibc 2.28), gcc-8 cross "
        "aarch64, com FDK-AAC 2.0.3 estatico; teto GLIBC_2.27",
    "nxsplash-nextos":
        "NXSplash do framework, copiada sem recompilar",
    "nxextract-ui":
        "UI de release do NXExtract, copiada sem recompilar",
}
PORT = "blossomtales"
LAUNCHER = "Blossom Tales.sh"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def elf_info(path):
    """NEEDED e SONAME reais do ELF -- lidos, nunca supostos."""
    try:
        out = subprocess.run(["aarch64-linux-gnu-readelf", "-d", path],
                             capture_output=True, text=True, check=True).stdout
    except Exception:
        return [], None
    needed, soname = [], None
    for line in out.splitlines():
        if "(NEEDED)" in line and "[" in line:
            needed.append(line.split("[", 1)[1].split("]", 1)[0])
        elif "(SONAME)" in line and "[" in line:
            soname = line.split("[", 1)[1].split("]", 1)[0]
    return sorted(set(needed)), soname


def classify(rel):
    """rel = caminho dentro da arvore gerada, com o dir do port na frente."""
    name = rel.split("/", 1)[1] if "/" in rel else rel
    if rel == LAUNCHER:
        return "launcher", "0755"
    if name == "nxport.json":
        return "nxbootstrap-config", "0644"
    if name in ("port.json", "gameinfo.xml"):
        return "portmaster-metadata", "0644"
    if name in ("LICENSE", "NOTICE.md"):
        return "license-notice", "0644"
    if name == "port-env.sh":
        return "script", "0644"
    if name == "extractor.json":
        return "nxextract-recipe", "0644"
    if name == "nxextract/nxextract.py":
        return "nxextract", "0644"
    if name == "nxextract/run-extractor.sh":
        return "nxextract-runner", "0644"
    if name == "nxextract/nxextract-runtime-env.sh":
        return "nxextract-runtime-env", "0644"
    if name == "nxextract/nxextract-ui":
        return "nxextract-ui-linux", "0755"
    if name == "nxsplash-nextos":
        return "nxsplash-linux", "0755"
    if name == PORT + "-nextos":
        return "project-linux", "0755"
    # `controllers.nxb` is the textual NXCONTROLLER_PROFILES/1 authority-3
    # bundle. Its suffix is shared with the binary nxruntime seed, but its
    # release kind must remain payload so the controller-profile closure can
    # validate the real header and pinned bytes.
    if name in ("controllers.nxb", "controllers-modern.nxb",
                "controllers-retro.nxb"):
        return "payload", "0644"
    if name.endswith(".nxb"):
        return "nxruntime-seed", "0644"
    if name.startswith(".nxruntime/"):
        if name.endswith("/runtime/" + PORT + "-nextos") or \
           name.endswith("/runtime/nxsplash-nextos") or \
           name.endswith("/runtime/nxextract/nxextract-ui"):
            return "nxruntime-generation-linux", "0755"
        return "nxruntime-generation", "0644"
    return "payload", "0644"


def main():
    tree, out_path = sys.argv[1], sys.argv[2]
    # O GENERATION.json e' a autoridade de path/mode/sha do fechamento gerado;
    # adivinhar o modo aqui produzia "artifact closure is stale".
    with open(os.path.join(tree, PORT, "GENERATION.json"), encoding="utf-8") as fh:
        generation = json.load(fh)
        closure = {a["path"]: a for a in generation["artifacts"]}
    package_version = open(
        os.path.join(tree, PORT, "version.txt"), encoding="utf-8"
    ).read().strip()
    bootstrap_version = generation["source_pins"]["nxbootstrap"]["version"]
    files = []
    for root, _dirs, names in os.walk(tree):
        for n in sorted(names):
            full = os.path.join(root, n)
            rel = os.path.relpath(full, tree)
            kind, mode = classify(rel)
            declared = closure.get(rel)
            if declared is not None:
                mode = declared["mode"]
            src = rel if rel == LAUNCHER else rel.split("/", 1)[1]
            rec = collections.OrderedDict((
                ("source", src), ("target", rel), ("kind", kind),
                ("mode", mode), ("sha256", sha256(full)),
            ))
            if kind.endswith("-linux"):
                # Todo ELF declara ABI, perfil, procedencia e o que REALMENTE
                # importa: os NEEDED lidos do proprio arquivo.
                needed, soname = elf_info(full)
                rec["architecture"] = "aarch64"
                rec["build_profile"] = "universal-low-glibc"
                # A copia guardada na generation store e' o MESMO byte do
                # componente vivo, entao tem de declarar a MESMA procedencia.
                base = rel.rsplit("/", 1)[-1]
                rec["provenance"] = PROVENANCE_BY_FILE.get(
                    base, "componente do framework, copiado sem recompilar")
                rec["needed"] = needed
                rec["soname"] = soname
            files.append(rec)
    files.sort(key=lambda r: r["target"])

    def digest(rel):
        return sha256(os.path.join(tree, PORT, rel))

    doc = collections.OrderedDict((
        ("schema_version", 2),
        ("source_root", "."),
        ("package", collections.OrderedDict((
            ("id", PORT),
            ("version", package_version),
            ("profile", "universal-portmaster"),
            ("launcher", LAUNCHER),
            ("launcher_chain", [LAUNCHER]),
            ("launcher_contract", collections.OrderedDict((
                ("generator", "nxbootstrap"),
                ("version", bootstrap_version),
                ("config_path", PORT + "/nxport.json"),
                ("config_sha256", digest("nxport.json")),
            ))),
            ("port_dir", PORT),
            ("license", collections.OrderedDict((
                ("spdx_id", "GPL-3.0-only"),
                ("source_url",
                 "https://github.com/NextOs-Ports/nextos_ports_android"),
                ("file", PORT + "/LICENSE"),
            ))),
        ))),
        ("release", collections.OrderedDict((
            ("source_date_epoch", 1756684800),
            ("max_glibc", "2.30"),
            ("compression", "deflated"),
        ))),
        ("nxextract", collections.OrderedDict((
            ("path", PORT + "/nxextract/nxextract.py"),
            ("version", "1.3.0"),
            ("minimum_version", "1.3.0"),
            ("sha256", digest("nxextract/nxextract.py")),
            ("runner_path", PORT + "/nxextract/run-extractor.sh"),
            ("runner_sha256", digest("nxextract/run-extractor.sh")),
            ("runtime_env_path", PORT + "/nxextract/nxextract-runtime-env.sh"),
            ("runtime_env_sha256", digest("nxextract/nxextract-runtime-env.sh")),
            ("recipe_path", PORT + "/extractor.json"),
            ("recipe_sha256", digest("extractor.json")),
            ("ui_path", PORT + "/nxextract/nxextract-ui"),
            ("ui_sha256", digest("nxextract/nxextract-ui")),
        ))),
        ("portmaster_metadata", collections.OrderedDict((
            ("port_json", collections.OrderedDict((
                ("path", PORT + "/port.json"),
                ("sha256", digest("port.json")),
            ))),
            ("gameinfo_xml", collections.OrderedDict((
                ("path", PORT + "/gameinfo.xml"),
                ("sha256", digest("gameinfo.xml")),
            ))),
            ("images", []),
        ))),
        # O ELF publico so depende da glibc do aparelho: sem SDL privada, sem
        # libstdc++ (o C++ do FDK-AAC entra estatico), sem RPATH/RUNPATH.
        ("dependencies", [
            collections.OrderedDict((
                ("namespace", "linux"), ("architecture", "aarch64"),
                ("soname", so), ("provider", "glibc-base"),
            )) for so in ("libc.so.6", "libdl.so.2", "libm.so.6",
                          "libpthread.so.0")
        ]),
        ("files", files),
    ))
    with open(out_path, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, indent=2, ensure_ascii=False)
        fh.write("\n")
    print("nxrelease.json: %d arquivos" % len(files))


if __name__ == "__main__":
    main()
