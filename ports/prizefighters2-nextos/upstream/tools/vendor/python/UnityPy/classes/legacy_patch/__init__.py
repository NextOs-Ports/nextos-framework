# PF2 only reads and rewrites Shader typetrees.  Avoid importing optional
# texture/audio helpers (Pillow, FMOD and platform codecs) on the handheld.
from .Shader import Shader as Shader

__all__ = ("Shader",)
