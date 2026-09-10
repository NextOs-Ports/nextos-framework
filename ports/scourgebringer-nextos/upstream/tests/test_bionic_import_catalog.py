#!/usr/bin/env python3
"""Keep old-glibc guest imports bound to explicit adapter providers."""

from pathlib import Path
import re
import subprocess
import sys


PORT = Path(__file__).resolve().parents[1]
CATALOG = PORT / "src" / "imports.gen.c"
EXPECTED = {
    "stat": "sdv_stat",
    "stat64": "sdv_stat",
    "lstat": "sdv_lstat",
    "lstat64": "sdv_lstat",
    "fstat": "sdv_fstat",
    "fstat64": "sdv_fstat",
    "fstatat": "sdv_fstatat",
    "fstatat64": "sdv_fstatat",
    "mknod": "sdv_mknod",
    "strlcpy": "sdv_strlcpy",
    "strlcat": "sdv_strlcat",
    "arc4random_buf": "sdv_arc4random_buf",
    "_ctype_": "sb_bionic_ctype",
    "sigaction": "my_sigaction",
    "sigemptyset": "my_sigemptyset",
    "sigfillset": "my_sigfillset",
    "sigaddset": "my_sigaddset",
    "sigdelset": "my_sigdelset",
    "sigismember": "my_sigismember",
    "sigprocmask": "my_sigprocmask",
    "pthread_sigmask": "my_pthread_sigmask",
    "sigsuspend": "my_sigsuspend",
    "sigpending": "my_sigpending",
}


def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")


source = CATALOG.read_text(encoding="utf-8")
entries = re.findall(
    r'\{"([^"\\]+)"\s*,\s*\(uintptr_t\)&([A-Za-z_][A-Za-z0-9_]*)\}',
    source,
)
catalog = {}
for name, target in entries:
    if name in catalog and catalog[name] != target:
        fail(f"conflicting providers for {name}: {catalog[name]} / {target}")
    catalog[name] = target

for name, target in EXPECTED.items():
    if catalog.get(name) != target:
        fail(f"{name} must bind explicitly to {target}")

observed = set()
for guest in (PORT / "libs" / "libmonodroid.so",
              PORT / "libs" / "libmonosgen-2.0.so",
              PORT / "libs" / "libSystem.Native.so"):
    if not guest.is_file():
        continue
    result = subprocess.run(
        ["readelf", "-Ws", str(guest)],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    )
    for line in result.stdout.splitlines():
        match = re.search(r"\bUND\s+(\S+)", line)
        if not match:
            continue
        name = match.group(1).split("@", 1)[0]
        if name in EXPECTED:
            observed.add(name)

for name in observed:
    if name not in catalog:
        fail(f"guest strong import remains unbound: {name}")

print(
    "bionic import catalog: explicit old-glibc providers OK"
    + (f"; observed guests={','.join(sorted(observed))}" if observed else "")
)
