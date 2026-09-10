import os

from jnius import autoclass


activity = autoclass("org.renpy.android.PythonSDLActivity").mActivity


def init():
    return None


def wakelock(enable):
    return None


def get_dpi():
    return float(os.environ.get("SUMMERTIME_DPI", "160"))


def vibrate(duration):
    return None


def check_permission(permission):
    return True


def request_permission(permission):
    return True

