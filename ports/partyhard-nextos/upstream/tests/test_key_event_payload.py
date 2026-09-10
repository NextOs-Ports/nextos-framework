#!/usr/bin/env python3
"""Fail closed if Android KeyEvents go back to one shared payload record.

Unity does not consume nativeInjectEvent synchronously: it keeps the jobject
and reads getAction/getKeyCode later on its own thread.  With a single shared
record, the second KeyEvent injected in one frame overwrote the first, so a
D-pad diagonal released in one frame lost one key-up and CharController kept
walking (measured on dArkOSRE, 05/09/2026).  Every injection must own its
payload, exactly like MotionEvent.obtain() clones already do.
"""
import re
from pathlib import Path

port = Path(__file__).resolve().parents[1]
source = (port / "src/jni.c").read_text()

for token in (
    "#define KEY_CLONE_COUNT 64",
    "static jobj *key_clones[KEY_CLONE_COUNT];",
    "static key_payload key_clone_data[KEY_CLONE_COUNT];",
    "unsigned slot = key_clone_next++ % KEY_CLONE_COUNT;",
    'key_clones[slot] = st_jni_keep(mk_object("android/view/KeyEvent"));',
    "key_clones[slot]->data = event;",
    "return key_clones[slot];",
    "event->down_time = key_down_time[keycode];",
):
    assert token in source, f"missing per-event KeyEvent payload contract: {token}"

# Both getters must read the payload of the object Unity is calling on.
for getter in ("j_KeyEvent_getInt", "j_KeyEvent_getLong"):
    match = re.search(r"static int64_t %s\(jctx \*c\)\n\{(.*?)\n\}" % getter, source, re.S)
    assert match, f"getter missing: {getter}"
    body = match.group(1)
    assert "key_from_object(c->self)" in body, f"{getter} ignores the object payload"
    assert "key_event." not in body, f"{getter} still reads a shared record"

# The shared object and the shared record must not return.
for forbidden in (
    "static jobj *key_event_object;",
    'key_event_object = mk_object("android/view/KeyEvent");',
    "} key_event;",
    "return key_event_object;",
):
    assert forbidden not in source, f"shared KeyEvent record returned: {forbidden}"

# The ring must outlive one frame of injections: the D-pad, face buttons,
# bumpers and START can all change in one poll, plus their releases.
count = int(re.search(r"#define KEY_CLONE_COUNT (\d+)", source).group(1))
assert count >= 32, "KeyEvent payload ring too small for one frame of transitions"

print("PASS: every injected KeyEvent owns its payload until Unity reads it")
