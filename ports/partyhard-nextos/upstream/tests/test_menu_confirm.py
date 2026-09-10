#!/usr/bin/env python3
"""Real owner/default loader and dispatcher: A confirms, R3 clicks."""
import os
import json
from pathlib import Path
import subprocess
import tempfile

port = Path(__file__).resolve().parents[1]
project = json.loads((port / "nxproject.json").read_text())
contract = json.loads((port / "adapter/adapter-contract.json").read_text())
assert contract["input"]["contexts"] == project["controls"]["contexts"]
assert project["controls"]["contexts"]["menu"]["A"] == "partyhard.action1"
fixture = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "input_gptk.h"
#include "nxinput_gptk.h"
static int confirms, clicks;
static int button(void *user, const char *action, int down, float value) {
    (void)user; (void)value;
    if (down && !strcmp(action, "partyhard.action1")) ++confirms;
    if (down && !strcmp(action, "partyhard.click")) ++clicks;
    return 0;
}
static int vector(void *user, const char *action, float x, float y) {
    (void)user; (void)action; (void)x; (void)y; return 0;
}
int main(int argc, char **argv) {
    assert(argc == 2 && !st_gptk_preinit(argv[1]) && st_gptk_loaded());
    const char *actions[] = {"partyhard.action1", "partyhard.action2", "partyhard.action3",
        "partyhard.action4", "partyhard.bumper_left", "partyhard.bumper_right",
        "partyhard.click", "partyhard.pause"};
    for (unsigned i = 0; i < sizeof actions / sizeof *actions; ++i)
        assert(!st_gptk_register_button(actions[i], actions[i], button, NULL));
    assert(!st_gptk_register_vector("partyhard.cursor", "cursor", vector, NULL));
    assert(!st_gptk_register_vector("partyhard.move", "move", vector, NULL));
    assert(!st_gptk_seal());
    st_gptk_set_context(ST_GPTK_CONTEXT_MENU, "test:pause");
    assert(st_gptk_feed_button(NXINPUT_GPTK_A, 1, 1) == ST_GPTK_LIVE_DELIVERED);
    assert(st_gptk_feed_button(NXINPUT_GPTK_A, 0, 0) == ST_GPTK_LIVE_DELIVERED);
    assert(confirms == 1 && clicks == 0);
    assert(st_gptk_feed_button(NXINPUT_GPTK_R3, 1, 1) == ST_GPTK_LIVE_DELIVERED);
    assert(st_gptk_feed_button(NXINPUT_GPTK_R3, 0, 0) == ST_GPTK_LIVE_DELIVERED);
    assert(confirms == 1 && clicks == 1);
    st_gptk_set_context(ST_GPTK_CONTEXT_GAMEPLAY, "test:gameplay");
    assert(st_gptk_feed_button(NXINPUT_GPTK_A, 1, 1) == ST_GPTK_LIVE_DELIVERED);
    assert(st_gptk_feed_button(NXINPUT_GPTK_A, 0, 0) == ST_GPTK_LIVE_DELIVERED);
    assert(confirms == 2 && clicks == 1);
    assert(!st_gptk_should_consume(NXINPUT_GPTK_R3));
    puts("PASS: actual default + live dispatcher; A remains native action/confirm, only R3 clicks in menus");
}
'''
vendor = port / "vendor/nxinput"
with tempfile.TemporaryDirectory(prefix="partyhard-confirm-test-", dir=os.getenv("TMPDIR")) as temp:
    c_file = Path(temp) / "test.c"
    binary = Path(temp) / "test"
    c_file.write_text(fixture)
    sources = sorted((vendor / "src").glob("nxinput_gptk*.c"))
    subprocess.run([os.getenv("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(port / "src"), "-I", str(vendor / "include"),
                    str(c_file), str(port / "src/input_gptk.c"),
                    *(str(path) for path in sources), str(vendor / "src/nxinput_sha256.c"),
                    "-lm", "-o", str(binary)], check=True)
    subprocess.run([str(binary), str(port)], check=True)
