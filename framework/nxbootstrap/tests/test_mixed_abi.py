#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Focused contract gate for nxbootstrap execution roles."""

import copy
import importlib.util
import json
import re
import struct
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GENERATOR_PATH = ROOT / "tools/generate-port.py"
EXAMPLE = ROOT / "examples/nxport-mixed-armv7.example.json"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def load_generator():
    spec = importlib.util.spec_from_file_location("nxbootstrap_generator", GENERATOR_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def elf_identity(path):
    header = path.read_bytes()[:20]
    require(header[:4] == b"\x7fELF" and len(header) == 20, "invalid ELF")
    require(header[5] == 1, "fixture is not little-endian")
    return header[4], struct.unpack_from("<H", header, 18)[0]


def write_elf_header(path, elf_class, machine):
    header = bytearray(20)
    header[:7] = b"\x7fELF" + bytes((elf_class, 1, 1))
    struct.pack_into("<H", header, 18, machine)
    path.write_bytes(header)
    path.chmod(0o700)


def expect_rejected(generator, document, fragment):
    try:
        generator.validate(document)
    except generator.ManifestError as error:
        require(fragment in str(error), "unexpected rejection: %s" % error)
    else:
        raise AssertionError("invalid execution role contract was accepted")


GNU_LD_SCRIPT = b"""/* GNU ld script
   Use the shared library, but some functions are only in
   the static library, so try that secondarily.  */
OUTPUT_FORMAT(elf32-littlearm)
GROUP ( /lib/arm-linux-gnueabihf/libc.so.6 /usr/lib/arm-linux-gnueabihf/libc_nonshared.a  AS_NEEDED ( /lib/ld-linux-armhf.so.3 ) )
"""


def build_arkos_root(base):
    """Reproduz a raiz ARMHF real de um ArkOS/dArkOS (Debian multiarch).

    Medido no aparelho: `/usr/lib/arm-linux-gnueabihf/libc.so` e um SCRIPT do
    GNU ld -- e o UNICO `.so` daquele diretorio que nao e ELF. A biblioteca de
    runtime e `libc.so.6`. `/lib/arm-linux-gnueabihf` e um link para a arvore
    de `/usr`.
    """
    usr = base / "usr/lib/arm-linux-gnueabihf"
    usr.mkdir(parents=True)
    (usr / "libc.so").write_bytes(GNU_LD_SCRIPT)
    for name in ("libc.so.6", "libSDL2-2.0.so.0", "libEGL.so.1",
                 "libGLESv2.so.2", "libasound.so.2",
                 "ld-linux-armhf.so.3"):
        write_elf_header(usr / name, 1, 40)
    lib = base / "lib"
    lib.mkdir(parents=True)
    (lib / "arm-linux-gnueabihf").symlink_to(usr)
    return usr


def closure_probe(functions, root, expected_class, expected_machine):
    return subprocess.run(
        [
            "bash", "-c",
            functions
            + '\nNXBOOTSTRAP_ROLE_LIBS=""\n'
            + 'nxbootstrap_add_execution_root splash "$1" "$2" "$3" firmware\n'
            + 'status=$?\n'
            + 'printf "status=%s libs=%s\\n" "$status" "$NXBOOTSTRAP_ROLE_LIBS"\n',
            "bash", str(root), str(expected_class), str(expected_machine),
        ],
        check=False, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )


def main():
    generator = load_generator()
    document = json.loads(EXAMPLE.read_text(encoding="utf-8"))
    config = generator.validate(copy.deepcopy(document))
    roles = config["execution_roles"]
    require(
        {
            name: role["architecture"]
            for name, role in roles.items()
            if name != "helpers"
        }
        == {"extractor": "aarch64", "splash": "armv7", "game": "armv7"},
        "mixed-ABI role topology changed",
    )

    with tempfile.TemporaryDirectory(prefix="nxbootstrap-mixed-abi.") as temporary:
        output = Path(temporary) / "generated"
        generator.generate(EXAMPLE, output, False)
        port = output / document["id"]
        launcher = (output / document["launcher_name"]).read_text(encoding="utf-8")
        syntax = subprocess.run(
            ["bash", "-n", str(output / document["launcher_name"])],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        require(syntax.returncode == 0, "mixed launcher shell syntax failed")
        canonical = json.loads((port / "nxport.json").read_text(encoding="utf-8"))
        require(canonical["execution_roles"] == roles, "role contract was not canonical")
        require(
            elf_identity(port / "nxsplash-nextos") == (1, 40),
            "mixed port did not receive the declared ARMHF splash",
        )
        for token in (
            "nxbootstrap_elf_identity()",
            'machine=$(( ${19} + (${20} * 256) ))',
            "EXECUTION RECEIPT: role=$role",
            "NXBOOTSTRAP_EXTRACTOR_EXECUTOR",
            "NXBOOTSTRAP_SPLASH_LOADER",
            "NXBOOTSTRAP_GAME_LOADER",
            '"$NXBOOTSTRAP_GAME_LOADER" --library-path',
            "/mnt/SDCARD/Persistent/.32bit_chroot/usr/lib",
            "/mnt/SDCARD/spruce/flip/muOS/usr/lib32",
            "reason=wrong-abi",
        ):
            require(token in launcher, "mixed launcher lacks %s" % token)
        require(
            "/mnt/SDCARD/spruce/flip/muOS/usr/lib/" not in launcher
            and "/mnt/SDCARD/spruce/flip/muOS/usr/lib:" not in launcher,
            "AArch64 muOS usr/lib contaminated the ARMHF route",
        )
        require(
            re.search(r"(^|[;&|\s])stat\s+-", launcher, re.MULTILINE) is None,
            "mixed launcher depends on external stat",
        )

        function_start = launcher.index("nxbootstrap_elf_identity()")
        function_end = launcher.index(
            "nxbootstrap_resolve_execution_role()", function_start
        )
        selector_functions = launcher[function_start:function_end]
        invalid_loader = Path(temporary) / "invalid-loader"
        invalid_loader.write_bytes(b"not an ELF")
        invalid_loader.chmod(0o700)
        wrong_loader = Path(temporary) / "wrong-loader"
        armhf_loader = Path(temporary) / "armhf-loader"
        write_elf_header(wrong_loader, 2, 183)
        write_elf_header(armhf_loader, 1, 40)
        selector = subprocess.run(
            [
                "bash",
                "-c",
                selector_functions
                + '\nNXBOOTSTRAP_ROUTE_LOADER=""\n'
                + 'nxbootstrap_select_execution_loader fixture 1 40 "$1" "$2" "$3"\n'
                + 'printf "selected=%s\\n" "$NXBOOTSTRAP_ROUTE_LOADER"\n',
                "bash",
                str(invalid_loader),
                str(wrong_loader),
                str(armhf_loader),
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        require(selector.returncode == 0, "valid ARMHF alternate loader was rejected")
        require(
            "reason=invalid-elf" in selector.stdout
            and "reason=wrong-abi" in selector.stdout
            and "selected=%s" % armhf_loader in selector.stdout,
            "alternate-loader selection did not reject invalid/wrong-class candidates",
        )
        # REGRESSAO DE CAMPO (dArkOS, 20/08/2026): o glob `libc.so*` casava o
        # SCRIPT do GNU ld `/usr/lib/arm-linux-gnueabihf/libc.so`, o launcher
        # o lia como ELF invalido e DESCARTAVA a closure ARMHF inteira:
        #   EXECUTION CANDIDATE REJECTED: role=splash ... reason=invalid-elf
        #   ERROR: execution role splash has no coherent armv7 closure
        # O ZIP nem chegava a abrir o NXExtract. Script de linker nao e
        # biblioteca de runtime: ele e IGNORADO. ELF de ABI errada continua
        # derrubando a raiz inteira.
        arkos = Path(temporary) / "arkos"
        arkos_root = build_arkos_root(arkos)
        result = closure_probe(selector_functions, arkos_root, 1, 40)
        require(result.returncode == 0,
                "ArkOS closure probe failed to run: %s" % result.stdout)
        require("status=0" in result.stdout,
                "the GNU ld script still destroys a valid ARMHF closure:\n%s"
                % result.stdout)
        require("reason=invalid-elf" not in result.stdout,
                "a GNU ld script is still reported as an invalid ELF:\n%s"
                % result.stdout)
        require("libs=%s" % arkos_root in result.stdout,
                "the real ARMHF root was not added to the closure:\n%s"
                % result.stdout)

        # O caminho por /lib/arm-linux-gnueabihf (link para /usr) vale igual.
        linked = closure_probe(selector_functions,
                               arkos / "lib/arm-linux-gnueabihf", 1, 40)
        require("status=0" in linked.stdout,
                "the multiarch symlink route was rejected:\n%s" % linked.stdout)

        # A propriedade que protege continua fechada: um ELF AArch64 no meio
        # da raiz ARMHF derruba a raiz inteira.
        poisoned = Path(temporary) / "poisoned"
        poisoned_root = build_arkos_root(poisoned)
        write_elf_header(poisoned_root / "libEGL.so.1", 2, 183)
        poison = closure_probe(selector_functions, poisoned_root, 1, 40)
        require("status=2" in poison.stdout and "reason=wrong-abi" in poison.stdout,
                "a mixed AArch64 ELF was accepted into the ARMHF closure:\n%s"
                % poison.stdout)

        # Uma raiz que so tem o script (nenhuma biblioteca de runtime) nao
        # vira closure: ela nao "conta" como raiz vista.
        empty = Path(temporary) / "script-only/usr/lib/arm-linux-gnueabihf"
        empty.mkdir(parents=True)
        (empty / "libc.so").write_bytes(GNU_LD_SCRIPT)
        script_only = closure_probe(selector_functions, empty, 1, 40)
        require("status=1" in script_only.stdout,
                "a root with only a linker script was accepted:\n%s"
                % script_only.stdout)

        absent = subprocess.run(
            [
                "bash",
                "-c",
                selector_functions
                + '\nNXBOOTSTRAP_ROUTE_LOADER=""\n'
                + 'nxbootstrap_select_execution_loader fixture 1 40 "$1"\n',
                "bash",
                str(Path(temporary) / "absent-loader"),
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        require(absent.returncode != 0, "absent alternate loader was accepted")

    wrong = copy.deepcopy(document)
    wrong["execution_roles"]["game"]["interpreter"] = (
        "/lib/ld-linux-aarch64.so.1"
    )
    expect_rejected(generator, wrong, "interpreter must be exactly")

    wrong = copy.deepcopy(document)
    wrong["execution_roles"]["game"]["architecture"] = "aarch64"
    wrong["execution_roles"]["game"]["interpreter"] = (
        "/lib/ld-linux-aarch64.so.1"
    )
    expect_rejected(generator, wrong, "game role must match")

    wrong = copy.deepcopy(document)
    wrong["execution_roles"]["extractor"]["executor"] = "native-or-loader"
    expect_rejected(generator, wrong, "native host NXExtract UI")

    wrong = copy.deepcopy(document)
    wrong["execution_roles"]["helpers"] = [
        {
            "id": "bad-helper",
            "architecture": "armv7",
            "executable": "helpers/bad",
            "executor": "native-or-loader",
            "interpreter": "/lib/ld-linux-armhf.so.3",
            "closure": "host",
        }
    ]
    expect_rejected(generator, wrong, "additional helpers cannot use the host closure")

    print(
        "nxbootstrap mixed-ABI gate passed: roles=3 helpers=0 "
        "splash=ARMHF closure=isolated wrong-abi=closed "
        "arkos-ld-script=ignored"
    )


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, ValueError, json.JSONDecodeError) as error:
        print("nxbootstrap mixed-ABI gate failed: %s" % error)
        raise SystemExit(1)
