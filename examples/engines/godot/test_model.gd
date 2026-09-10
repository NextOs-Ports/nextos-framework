# SPDX-License-Identifier: GPL-3.0-only
extends SceneTree
func _init():
    var model = load("res://Model.gd").new()
    if model.step(Vector2(1,0), 1):
        quit(1)
        return
    for _i in range(40):
        if not model.step(Vector2(1,0), 0.1):
            quit(2)
            return
    if model.score != 1:
        quit(3)
        return
    print("PASS: original Godot model; headless test, no hardware claim")
    quit(0)
