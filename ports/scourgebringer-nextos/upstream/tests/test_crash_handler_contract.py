#!/usr/bin/env python3
"""Keep the fatal-signal reporter non-recursive and async-signal-safe."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "main.c").read_text(encoding="utf-8")

handler_start = SOURCE.index("static void crash_handler(")
handler_end = SOURCE.index("\n}\nstatic void install_crash_handler", handler_start) + 2
handler = SOURCE[handler_start:handler_end]

for required in (
    "g_crash_in_progress",
    "crash_write_all(g_crash_buffer, length)",
    "_exit(128 + sig)",
    "u->uc_mcontext.pc",
):
    assert required in handler, f"missing safe crash-handler primitive: {required}"

for forbidden in (
    "fprintf(",
    "printf(",
    "snprintf(",
    "fflush(",
    "fopen(",
    "dladdr(",
    "/proc/self/maps",
    "uintptr_t *)sp",
    "uintptr_t *)fp",
):
    assert forbidden not in handler, f"unsafe crash-handler operation: {forbidden}"

install_start = SOURCE.index("static void install_crash_handler(")
install_end = SOURCE.index("\n}\n", install_start) + 2
install = SOURCE[install_start:install_end]
for required in ("sigaltstack", "SA_ONSTACK", "SA_RESETHAND", "sigfillset"):
    assert required in install, f"missing crash-handler hardening: {required}"

print("crash handler contract: alternate stack + async-safe first-fault log OK")
