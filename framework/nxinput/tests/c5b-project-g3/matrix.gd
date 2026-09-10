extends SceneTree
# SPDX-License-Identifier: GPL-3.0-only
# V4-CONTROLLERS-03 / C5B: the Godot 3 side of the functional matrix.
#
# It reports what EACH real consumer route saw, and it never touches the
# mapping: the seam inside JoypadLinux::open_joypad() already decided, before
# this script existed, whether the pad could be announced at all.
#
# Three routes, kept separate on purpose:
#   input   InputEventJoypadButton / InputEventJoypadMotion through _input
#   poll    Input.is_joy_button_pressed / Input.get_joy_axis
#   action  InputMap actions bound to the engine's OWN named constants
#
# The exit chord is read from the ENGINE's binding, not from a number this
# script chose: JOY_SELECT and JOY_START are Godot 3's own enum, so whatever
# the mapping bound to `back` and `start` is what fires it.

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
				e.button_index, " pressed=", int(e.pressed))
		elif e is InputEventJoypadMotion:
			print("MX EV route=input kind=motion dev=", e.device, " axis=",
				e.axis, " v=", String(e.axis_value).substr(0, 7))
		elif e is InputEventKey:
			# Nothing in this contract may synthesise a keyboard event. If
			# one appears, the run has to say so.
			owner_tree.keyboard_events += 1
			print("MX EV route=input kind=KEY scancode=", e.scancode,
				" pressed=", int(e.pressed))

func _pads():
	return Input.get_connected_joypads()

func _init():
	save_path = OS.get_environment("NXC5B_SAVE")
	print("MX engine=godot3 version=", Engine.get_version_info()["string"])
	print("MX pid=", OS.get_process_id())
	print("MX headless_backend=", OS.get_name(), " can_draw=",
		int(OS.can_draw()))
	var pads = _pads()
	print("MX pads=", pads)
	for id in pads:
		print("MX pad id=", id, " guid=", Input.get_joy_guid(id), " name=",
			Input.get_joy_name(id))
	# InputMap route. Every action names the ENGINE's constant, so the index
	# it watches is the engine's, never one this script invented.
	var acts = {
		"mx_a": JOY_XBOX_A, "mx_b": JOY_XBOX_B, "mx_x": JOY_XBOX_X,
		"mx_y": JOY_XBOX_Y, "mx_l1": JOY_L, "mx_r1": JOY_R,
		"mx_l2": JOY_L2, "mx_r2": JOY_R2, "mx_l3": JOY_L3, "mx_r3": JOY_R3,
		"mx_start": JOY_START, "mx_select": JOY_SELECT,
		"mx_guide": JOY_GUIDE,
		"mx_dpup": JOY_DPAD_UP, "mx_dpdown": JOY_DPAD_DOWN,
		"mx_dpleft": JOY_DPAD_LEFT, "mx_dpright": JOY_DPAD_RIGHT
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
	get_root().add_child(listener)
	listener.set_process_input(true)
	var seconds = int(OS.get_environment("NXC5B_SECONDS"))
	if seconds <= 0:
		seconds = 60
	deadline = OS.get_ticks_msec() + seconds * 1000
	print("MX chord_binding select=", JOY_SELECT, " start=", JOY_START)
	print("MX ready")

func _iteration(delta):
	var pads = _pads()
	var report = []
	for dev in pads:
		var btns = []
		for b in range(0, 17):
			if Input.is_joy_button_pressed(dev, b):
				btns.append(b)
		var axs = []
		for a in range(0, 10):
			var v = Input.get_joy_axis(dev, a)
			if abs(v) > 0.20:
				axs.append(String(a) + "=" + String(v).substr(0, 7))
		report.append(String(dev) + ":btn" + String(btns) + ":ax" + String(axs))
	var acts = []
	for a in InputMap.get_actions():
		if String(a).begins_with("mx_") and Input.is_action_pressed(a):
			acts.append(a)
	var line = "MX POLL pads=" + String(pads) + " dev=" + String(report) \
		+ " act=" + String(acts)
	if line != last_poll:
		print(line)
		last_poll = line
	# The chord is SELECT+START on the SAME pad, through the engine's own
	# binding. A press on two different pads must not fire it.
	if chord_fired == 0:
		for dev in pads:
			if Input.is_joy_button_pressed(dev, JOY_SELECT) and \
					Input.is_joy_button_pressed(dev, JOY_START):
				chord_fired = 1
				print("MX CHORD fired dev=", dev)
				_finalise("chord")
				quit()
				return true
	if OS.get_ticks_msec() >= deadline:
		print("MX done reason=deadline")
		_finalise("deadline")
		quit()
		return true
	return false

func _finalise(reason):
	# One and only one finalisation, whichever way the run ends.
	if finalised > 0:
		print("MX FINALISE-DUPLICATE reason=", reason)
		return
	finalised += 1
	if save_path != "":
		var f = File.new()
		if f.open(save_path, File.WRITE) == OK:
			f.store_line("reason=" + reason)
			f.store_line("keyboard_events=" + String(keyboard_events))
			f.close()
	print("MX FINALISE reason=", reason, " count=", finalised,
		" keyboard_events=", keyboard_events)

func _finalize():
	# Reached on SIGTERM too: the engine turns the signal into a quit and the
	# main loop is torn down through here.
	_finalise("lifecycle")
	print("MX EXIT finalised=", finalised, " keyboard_events=",
		keyboard_events)
