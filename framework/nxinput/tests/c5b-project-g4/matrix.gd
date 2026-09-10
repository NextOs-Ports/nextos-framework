extends SceneTree
# SPDX-License-Identifier: GPL-3.0-only
# V4-CONTROLLERS-03 / C5B: the Godot 4 side of the functional matrix.
#
# Same three routes and the same chord rule as the Godot 3 script, written in
# Godot 4's dialect. The engine's own JoyButton constants are what the chord
# and the InputMap actions name, so the indices are the ENGINE's -- and they
# genuinely differ from Godot 3's, which is exactly why both majors are run.

var listener = null
var deadline = 0
var last_poll = ""
var save_path = ""
var finalised = 0
var chord_fired = 0
var keyboard_events = 0

class L extends Node:
	var owner_tree = null
	func _input(e):
		if e is InputEventJoypadButton:
			print("MX EV route=input kind=button dev=", e.device, " idx=",
				int(e.button_index), " pressed=", int(e.pressed))
		elif e is InputEventJoypadMotion:
			print("MX EV route=input kind=motion dev=", e.device, " axis=",
				int(e.axis), " v=", str(e.axis_value).substr(0, 7))
		elif e is InputEventKey:
			owner_tree.keyboard_events += 1
			print("MX EV route=input kind=KEY keycode=", int(e.keycode),
				" pressed=", int(e.pressed))

func _initialize():
	save_path = OS.get_environment("NXC5B_SAVE")
	print("MX engine=godot4 version=", Engine.get_version_info()["string"])
	print("MX pid=", OS.get_process_id())
	# This is runtime proof from the engine itself, not an assertion made by
	# the Python driver. The gate requires headless + zero screens on every
	# Godot 4 scenario and the process receives no desktop-display variables.
	print("MX display_server=", DisplayServer.get_name(), " screens=",
		DisplayServer.get_screen_count())
	var pads = Input.get_connected_joypads()
	print("MX pads=", pads)
	for id in pads:
		print("MX pad id=", id, " guid=", Input.get_joy_guid(id), " name=",
			Input.get_joy_name(id))
	var acts = {
		"mx_a": JOY_BUTTON_A, "mx_b": JOY_BUTTON_B, "mx_x": JOY_BUTTON_X,
		"mx_y": JOY_BUTTON_Y,
		"mx_l1": JOY_BUTTON_LEFT_SHOULDER,
		"mx_r1": JOY_BUTTON_RIGHT_SHOULDER,
		"mx_l3": JOY_BUTTON_LEFT_STICK, "mx_r3": JOY_BUTTON_RIGHT_STICK,
		"mx_start": JOY_BUTTON_START, "mx_select": JOY_BUTTON_BACK,
		"mx_guide": JOY_BUTTON_GUIDE,
		"mx_dpup": JOY_BUTTON_DPAD_UP, "mx_dpdown": JOY_BUTTON_DPAD_DOWN,
		"mx_dpleft": JOY_BUTTON_DPAD_LEFT,
		"mx_dpright": JOY_BUTTON_DPAD_RIGHT
	}
	for a in acts.keys():
		if not InputMap.has_action(a):
			InputMap.add_action(a)
			for dev in range(0, 4):
				var ev = InputEventJoypadButton.new()
				ev.device = dev
				ev.button_index = acts[a]
				InputMap.action_add_event(a, ev)
	listener = L.new()
	listener.owner_tree = self
	root.add_child(listener)
	listener.set_process_input(true)
	var seconds = int(OS.get_environment("NXC5B_SECONDS"))
	if seconds <= 0:
		seconds = 60
	deadline = Time.get_ticks_msec() + seconds * 1000
	print("MX chord_binding select=", int(JOY_BUTTON_BACK), " start=",
		int(JOY_BUTTON_START))
	print("MX ready")

func _process(delta):
	OS.delay_msec(8)
	var pads = Input.get_connected_joypads()
	var report = []
	for dev in pads:
		var btns = []
		for b in range(0, 17):
			if Input.is_joy_button_pressed(dev, b):
				btns.append(b)
		var axs = []
		for a in range(0, 10):
			var v = Input.get_joy_axis(dev, a)
			if absf(v) > 0.20:
				axs.append(str(a) + "=" + str(v).substr(0, 7))
		report.append(str(dev) + ":btn" + str(btns) + ":ax" + str(axs))
	var acts = []
	for a in InputMap.get_actions():
		if str(a).begins_with("mx_") and Input.is_action_pressed(a):
			acts.append(a)
	var line = "MX POLL pads=" + str(pads) + " dev=" + str(report) \
		+ " act=" + str(acts)
	if line != last_poll:
		print(line)
		last_poll = line
	if chord_fired == 0:
		for dev in pads:
			if Input.is_joy_button_pressed(dev, JOY_BUTTON_BACK) and \
					Input.is_joy_button_pressed(dev, JOY_BUTTON_START):
				chord_fired = 1
				print("MX CHORD fired dev=", dev)
				_finalise("chord")
				return true
	if Time.get_ticks_msec() >= deadline:
		print("MX done reason=deadline")
		_finalise("deadline")
		return true
	return false

func _finalise(reason):
	if finalised > 0:
		print("MX FINALISE-DUPLICATE reason=", reason)
		return
	finalised += 1
	if save_path != "":
		var f = FileAccess.open(save_path, FileAccess.WRITE)
		if f != null:
			f.store_line("reason=" + reason)
			f.store_line("keyboard_events=" + str(keyboard_events))
			f.close()
	print("MX FINALISE reason=", reason, " count=", finalised,
		" keyboard_events=", keyboard_events)

func _finalize():
	_finalise("lifecycle")
	print("MX EXIT finalised=", finalised, " keyboard_events=",
		keyboard_events)
