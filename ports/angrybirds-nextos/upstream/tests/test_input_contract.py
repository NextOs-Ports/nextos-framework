#!/usr/bin/env python3
"""Fail closed if the approved Mali-450 global-cursor contract regresses."""

from pathlib import Path


source = (Path(__file__).resolve().parents[1] / "src" / "ab_input.c").read_text(
    encoding="utf-8"
)
lua = (Path(__file__).resolve().parents[1] / "src" /
       "ab_slingshot_adapter.lua").read_text(encoding="utf-8")
evdev = (Path(__file__).resolve().parents[1] / "src" /
         "ab_evdev_exit.c").read_text(encoding="utf-8")
pump = source[source.index("void ab_input_pump(void)") :
              source.index("void ab_input_flush(void)")]

checks = {
    "no gameplay-only cursor mode": "NXINPUT_CURSOR_GAMEPLAY" not in source,
    "right stick is not camera pan": "g_camera_pan" not in source,
    "global cursor context exists":
        "nxinput_set_cursor_context(g_input, NXINPUT_CURSOR_MENU);" in pump,
    "cursor button edges run before gameplay branch":
        pump.index("cursor_button_edges(") < pump.index("if (gameplay)"),
    "global cursor update runs after gameplay branch":
        pump.index("nxinput_set_cursor_context(g_input, NXINPUT_CURSOR_MENU);")
        > pump.index("if (gameplay)"),
    "A and R3 share global pointer authority":
        "if (button == NXINPUT_BUTTON_A ||\n"
        "        button == NXINPUT_BUTTON_RIGHT_STICK)\n"
        "      continue;" in source,
    "cursor hides after approved idle timeout":
        "SDL_GetTicks() - g_cursor_seen > 2000" in source,
    "cursor visibility is observable":
        'ab_log("[input] cursor visivel")' in source
        and 'ab_log("[input] cursor oculto por inatividade")' in source,
    "A launch is routed only from an existing sling owner":
        "if (sling_owned_before &&\n"
        "        (pressed & NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_A)) != 0u)\n"
        "      ab_lua_control_press_launch();" in pump,
    "Lua proves a real drag before precision launch":
        "if pressLaunch and controlDragging and self:isBirdDragged() then" in lua,
    "stick release fallback remains available":
        "if magnitude < 0.12 then\n"
        "      if self:isBirdDragged() then\n"
        "        self:releaseRubberBandToLaunchBird()" in lua,
    "H700-class SDL GUIDE fallback is exit-only":
        "NXINPUT_BUTTON_GUIDE" in pump
        and 'request_quit("SELECT+START SDL GUIDE")' in pump,
    "evdev fallback uses stable kernel keycodes":
        all(token in evdev for token in (
            "EVIOCGKEY", "BTN_SELECT", "BTN_START",
            "BTN_TRIGGER_HAPPY1", "BTN_TRIGGER_HAPPY2")),
    "evdev exit survives a missing SDL controller":
        pump.index("ab_evdev_exit_poll(now)") < pump.index("if (!g_input)"),
    "all controller exit routes request clean shutdown":
        'request_quit("SELECT+START evdev")' in pump
        and 'request_quit("SELECT+START SDL BACK")' in pump,
}

failed = [name for name, passed in checks.items() if not passed]
if failed:
    raise SystemExit("angrybirds input contract: FAIL: " + "; ".join(failed))

print("angrybirds input contract: PASS global-cursor=1 gameplay=1 "
      "precision-A=1 exit-fallbacks=3 idle-hide=2s")
