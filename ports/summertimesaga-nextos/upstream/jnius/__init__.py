import os


class JavaException(Exception):
    pass


class _Dummy:
    def __init__(self, name="dummy"):
        self._name = name

    def __call__(self, *args, **kwargs):
        return _Dummy(self._name)

    def __getattr__(self, name):
        if name.startswith("is"):
            return lambda *args, **kwargs: False
        if name.startswith(("finish", "hide", "arm", "open", "request", "start")):
            return lambda *args, **kwargs: None
        return _Dummy("%s.%s" % (self._name, name))

    def __bool__(self):
        return False

    def __str__(self):
        return ""


class _PackageManager:
    def hasSystemFeature(self, feature):
        return False


class _Activity:
    def getPackageManager(self):
        return _PackageManager()

    def hidePresplash(self):
        return None

    def armOnStop(self):
        return None

    def finishOnStop(self):
        return None

    def finishAndRemoveTask(self):
        return None

    def openEditor(self, filename):
        return None

    def requestPermission(self, permission):
        return True

    def checkPermission(self, permission):
        return True


class _Build:
    MANUFACTURER = "NextOS"
    MODEL = "Amlogic-old"


class _PythonSDLActivity:
    mActivity = _Activity()

    @staticmethod
    def isChromebook():
        return False


class _SDLActivity:
    mHasFocus = True


class _System:
    @staticmethod
    def exit(code=0):
        raise SystemExit(code)


class _Locale:
    @staticmethod
    def getDefault():
        return _Locale()

    def getLanguage(self):
        return os.environ.get("LANG", "en")[:2] or "en"


class _TextToSpeech:
    QUEUE_FLUSH = 0

    def __init__(self, *args, **kwargs):
        pass

    def speak(self, *args, **kwargs):
        return 0

    def stop(self):
        return 0

    def shutdown(self):
        return 0


_CLASSES = {
    "android.os.Build": _Build,
    "org.renpy.android.PythonSDLActivity": _PythonSDLActivity,
    "org.libsdl.app.SDLActivity": _SDLActivity,
    "java.lang.System": _System,
    "java.util.Locale": _Locale,
    "android.speech.tts.TextToSpeech": _TextToSpeech,
}


def autoclass(name, *args, **kwargs):
    return _CLASSES.get(name, type(name.split(".")[-1], (_Dummy,), {}))


def cast(destclass, obj):
    return obj


def detach():
    return None


class PythonJavaClass:
    pass


def java_method(*args, **kwargs):
    def deco(fn):
        return fn
    return deco

