#!/usr/bin/env python3
"""Exercise the adapter's provider choice with real host dlopen and fake drivers.

This is a library search/override regression, not physical GPU validation.
No DRM node, display, game data or graphics hardware is accessed.
"""

from pathlib import Path
import os
import shlex
import subprocess
import tempfile


PORT = Path(__file__).resolve().parents[2]
DRIVERS = ("SDL_VIDEO_EGL_DRIVER", "SDL_VIDEO_GL_DRIVER")
SONAMES = ("libEGL.so.1", "libGLESv2.so.2")
ADAPTER = (PORT / "adapter-env.sh").read_text()
START = ADAPTER.index('if [ "$NX_TEARSCAPE_DRM_OK" = 1 ]; then')
END = ADAPTER.index("# Video provider by capability.", START)
BLOCK = ADAPTER[START:END]


def run(arguments, **kwargs):
    return subprocess.run(arguments, check=True, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          **kwargs).stdout


def selection(overrides=None, drm=True):
    environment = os.environ.copy()
    for key in (*DRIVERS, "NX_TEARSCAPE_SDL_EGL"):
        environment.pop(key, None)
    environment["NX_TEARSCAPE_DRM_OK"] = "1" if drm else "0"
    environment.update(overrides or {})
    # Execute the exact production block while supplying its already-measured
    # DRM capability; this never creates or probes a host /dev/dri node.
    output = run(["bash", "-eu", "-c", BLOCK + '''
printf '%s\\n' "${SDL_VIDEO_EGL_DRIVER-<unset>}" \
  "${SDL_VIDEO_GL_DRIVER-<unset>}" "${NX_TEARSCAPE_SDL_EGL-<unset>}"
'''], env=environment)
    return tuple(output.splitlines())


def main():
    assert selection() == (*SONAMES, "1"), "automatic pair differs"
    assert selection(dict.fromkeys(DRIVERS, "")) == (*SONAMES, "1")
    assert selection(drm=False) == ("<unset>", "<unset>", "<unset>")
    common = (PORT / "src/shim/nx_common.c").read_text()
    for soname in SONAMES:
        assert f'load("{soname}",' in common, "shim/SDL resolver divergence"

    with tempfile.TemporaryDirectory(prefix="tearscape-provider-") as work:
        root = Path(work)
        current, stale, shim = [root / name for name in
                                ("firmware-local", "firmware-system", "shim")]
        for directory in (current, stale, shim):
            directory.mkdir()
        source = root / "provider.c"
        source.write_text('const char *provider_identity(void) { return IDENTITY; }\n')
        compiler = shlex.split(os.environ.get("HOST_CC", "cc"))
        for directory, identity in ((current, "compatible-11.7"),
                                    (stale, "incompatible-10.6"),
                                    (shim, "translation-shim")):
            for unversioned, soname, shim_soname in (
                ("libEGL.so", SONAMES[0], "libtearscape-egl-shim.so"),
                ("libGLESv2.so", SONAMES[1], "libtearscape-gles2-shim.so"),
            ):
                run(compiler + ["-shared", "-fPIC", '-DIDENTITY="' + identity + '"',
                                "-Wl,-soname," + (shim_soname if directory == shim else soname),
                                str(source), "-o", str(directory / unversioned)])
                if directory != shim:
                    (directory / soname).symlink_to(unversioned)

        probe_source = root / "probe.c"
        probe_source.write_text(r'''#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
static void inspect(const char *path) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); exit(2); }
    const char *(*identity)(void) = dlsym(h, "provider_identity");
    if (!identity) exit(3);
    puts(identity());
}
int main(int argc, char **argv) {
    if (argc != 5) return 4;
    for (int i = 1; i < argc; ++i) inspect(argv[i]);
    return 0;
}
''')
        probe = root / "probe"
        run(compiler + [str(probe_source), "-ldl", "-o", str(probe)])
        environment = os.environ.copy()
        environment.pop("LD_PRELOAD", None)
        environment["LD_LIBRARY_PATH"] = ":".join(map(str, (current, stale, shim)))

        def resolve(pair):
            # Godot loads its absolute translation shims first. Their private
            # SONAME must not capture the subsequent firmware lookups.
            return run([str(probe), str(shim / "libEGL.so"),
                        str(shim / "libGLESv2.so"), *pair],
                       env=environment).splitlines()

        old_pair = (str(stale / "libEGL.so"), str(stale / "libGLESv2.so"))
        assert resolve(old_pair) == ["translation-shim"] * 2 + ["incompatible-10.6"] * 2
        assert resolve(selection()[:2]) == ["translation-shim"] * 2 + ["compatible-11.7"] * 2
        overrides = dict(zip(DRIVERS, old_pair))
        assert selection(overrides) == (*old_pair, "1"), "complete override replaced"
        assert resolve(selection(overrides)[:2])[-2:] == ["incompatible-10.6"] * 2
        assert selection({DRIVERS[0]: old_pair[0]}) == (old_pair[0], "<unset>", "1")
        assert selection({DRIVERS[1]: old_pair[1]}) == ("<unset>", old_pair[1], "1")
        assert selection(overrides, drm=False) == (*old_pair, "<unset>")

    print("FIRMWARE PROVIDER HOST: PASS (loader order, shim isolation, overrides, non-DRM)")


if __name__ == "__main__":
    main()
