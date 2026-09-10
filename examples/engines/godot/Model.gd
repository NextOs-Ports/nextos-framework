# SPDX-License-Identifier: GPL-3.0-only
extends Reference
var position = Vector2(40, 240)
var score = 0
func step(motion, delta):
    if delta < 0 or delta > 0.25:
        return false
    position += motion.clamped(1.0) * 120 * delta
    position.x = clamp(position.x, 8, 632)
    position.y = clamp(position.y, 8, 472)
    if position.distance_to(Vector2(520, 240)) < 12:
        score += 1
        position = Vector2(40, 240)
    return true
