# SPDX-License-Identifier: GPL-3.0-only
extends Node2D
var model = preload("res://Model.gd").new()
func _ready():
    var save = File.new()
    if save.file_exists("user://training-score.txt") and save.open("user://training-score.txt", File.READ) == OK:
        model.score = int(save.get_line())
        save.close()
func _process(delta):
    var motion = Vector2(Input.get_action_strength("ui_right") - Input.get_action_strength("ui_left"), Input.get_action_strength("ui_down") - Input.get_action_strength("ui_up"))
    model.step(motion, min(delta, 0.25))
    update()
    if Input.is_action_just_pressed("ui_cancel"):
        var save = File.new()
        if save.open("user://training-score.txt", File.WRITE) != OK:
            push_error("Save failed / Falha ao salvar")
            return
        save.store_line(str(model.score))
        save.close()
        get_tree().quit()
func _draw():
    draw_rect(Rect2(0,0,640,480), Color(0.08,0.12,0.18))
    draw_circle(Vector2(520,240), 10, Color(1,0.7,0.2))
    draw_rect(Rect2(model.position-Vector2(6,6),Vector2(12,12)), Color(0.2,0.8,1))
