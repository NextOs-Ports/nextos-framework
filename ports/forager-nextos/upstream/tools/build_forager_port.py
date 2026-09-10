#!/usr/bin/env python3
"""Build the deterministic, STORE-only Forager runtime archive from the owner APK."""

import hashlib
import os
import stat
import sys
import zipfile


SOURCE_MEMBERS = (
    "assets/BlitworksCloudSave.ext",
    "assets/GooglePlayBillingExtension.ext",
    "assets/audiogroup1.dat",
    "assets/consentform.html",
    "assets/data.txt",
    "assets/game.droid",
    "assets/humblebundle_h264_nopreroll.mp4",
    "assets/local/chinese.json",
    "assets/local/chinese_traditional.json",
    "assets/local/english.json",
    "assets/local/french.json",
    "assets/local/german.json",
    "assets/local/japanese.json",
    "assets/local/korean.json",
    "assets/local/portuguese.json",
    "assets/local/russian.json",
    "assets/local/spanish.json",
    "assets/local/thai.json",
    "assets/local/turkish.json",
    "assets/options.ini",
    "assets/portrait_splash.png",
    "assets/splash.png",
    "lib/armeabi-v7a/libc++_shared.so",
    "lib/armeabi-v7a/libyoyo.so",
)

# Validate the game payload instead of the outer APK bytes. This deliberately
# accepts legitimate resigning, ZIP metadata/compression changes and extra
# store files while keeping every byte consumed by the runner pinned.
SOURCE_PROOFS = {
    "assets/BlitworksCloudSave.ext": (0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
    "assets/GooglePlayBillingExtension.ext": (0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
    "assets/audiogroup1.dat": (25139091, "703a31a4a00b723a552aef41f080b88020e0b66f6a6684fd820cbdad33d326e5"),
    "assets/consentform.html": (27122, "e84d4254ad60362e46a7f2ee254c30d013599da2e5bb0d12eecfed301c1a71ae"),
    "assets/data.txt": (9288, "065e45969ff6e90d167ba673756b91977f27c67b786539b5f9d9859ada70caef"),
    "assets/game.droid": (108980424, "a1f2e44e6dcf4d6073097bfa4796ee200d2f170d9ca26e29983499889107f9f7"),
    "assets/humblebundle_h264_nopreroll.mp4": (5217134, "65234c76e3749e2c2906b17e088ca3a115de17af2cc8a149d30a78c3aa4c5782"),
    "assets/local/chinese.json": (88916, "309c23ae74bfc065021cfc747162c57a8e6d8f0bd176d68ed18342f09232971f"),
    "assets/local/chinese_traditional.json": (89807, "45933474a2408ccf98d35e0e7749ae8e67000a5637087be274363d7d4a832356"),
    "assets/local/english.json": (90233, "a0468b638d99f8acd504697912ed632aa072109c8be3f9eb99eeba1b87816cd8"),
    "assets/local/french.json": (101915, "dac4654e06a092bba227e2d168e27d35c28e1343cbf68a0776fd91cb80889287"),
    "assets/local/german.json": (96282, "9b4e7f3fc880f4ce4aadfa07238e618f3874825e9b5e2783bcac6353795b3b29"),
    "assets/local/japanese.json": (103640, "1f07c860136d5f747bc3b8501fc0642871c320e6363d4a649d3c67e766cf3eef"),
    "assets/local/korean.json": (103344, "cf7f97ec295e73a4725443b7d03dc36f4d47d2a1c4b65e2bc53d4eda5605b4a0"),
    "assets/local/portuguese.json": (97513, "2bc0a2fde363e4fa61062fb08e9db3a9428b8d25597d8c5d7260a549720d17c5"),
    "assets/local/russian.json": (134341, "84ef8901bb6971ee5d44b33dbda18814a86659f0f47bc1318c6b7dcba180e68c"),
    "assets/local/spanish.json": (99141, "4aa0ea8e8f4f1ba75c6648ff1da3e0df15108b1f7c3b0772ceef919f4d286799"),
    "assets/local/thai.json": (164979, "03974a776984820da0b817c2fb57ef41bb1451fb64e40e29f54ae68d54fe9b8f"),
    "assets/local/turkish.json": (94537, "96007e487b2071561efb099286d64927cde20629c50bc57ba84955b533771c35"),
    "assets/options.ini": (910, "f0c9ea4abbd7ebc8b9f76c9c4e162acec82fb7ededa8cc2440a8787bdae051cf"),
    "assets/portrait_splash.png": (662265, "52951bbff230e7ea93f296271c0d3dd1d31b2f798da6ff609a1f975633fe1d96"),
    "assets/splash.png": (339456, "68a7cbead51d5abcec31c951a5390a734f41022cd74a49a7f36e7b40bebbb57d"),
    "lib/armeabi-v7a/libc++_shared.so": (554808, "8480ac1f1fcedb6d05eec0519afa0db19b84541ed1bbaeb1f7b7dacf20008d95"),
    "lib/armeabi-v7a/libyoyo.so": (32414496, "6aa08df5ed5f8144da77aba71ee04c704996e7ea635e40926e256090eb583fb1"),
}

INJECTED_MEMBERS = {
    "assets/mods/settings.json": b'{"loadOrder":[]}\n',
    "assets/gamecontrollerdb.txt": (
        b"# Minimal mappings proven by the Forager NextOS adapter.\n"
        b"03000000100800000100000010010000,Twin PlayStation Adapter,a:b2,b:b1,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,leftshoulder:b6,leftstick:b10,lefttrigger:b4,leftx:a0,lefty:a1,rightshoulder:b7,rightstick:b11,righttrigger:b5,rightx:a3,righty:a2,start:b9,x:b3,y:b0,platform:Linux,\n"
        b"03000000100800000300000010010000,USB Gamepad,a:b2,b:b1,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,leftshoulder:b6,leftstick:b10,lefttrigger:b4,leftx:a0,lefty:a1,rightshoulder:b7,rightstick:b11,righttrigger:b5,rightx:a3,righty:a2,start:b9,x:b3,y:b0,platform:Linux,\n"
        b"0300ef68bc2000000055000011010000,Twin USB Joystick,dpleft:h0.8,rightx:a2,dpright:h0.2,rightshoulder:b7,dpdown:h0.4,righty:a3,leftshoulder:b6,y:b4,x:b3,b:b1,a:b0,dpup:h0.1,back:b10,leftstick:b13,start:b11,lefty:a1,lefttrigger:b8,righttrigger:b9,rightstick:b14,leftx:a0,platform:Linux,\n"
        b"190000004b4800000011000000010000,GO-Super Gamepad,a:b0,b:b1,back:b12,dpdown:b9,dpleft:b10,dpright:b11,dpup:b8,guide:b16,leftshoulder:b4,leftstick:b14,lefttrigger:b6,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b15,righttrigger:b7,rightx:a2,righty:a3,start:b13,x:b3,y:b2,platform:Linux,\n"
    ),
}

MAX_MEMBER = 128 * 1024 * 1024
CHUNK = 1024 * 1024


def fail(message):
    raise SystemExit("forager-port error: %s" % message)


def inside(root, path):
    try:
        return os.path.commonpath((root, path)) == root
    except ValueError:
        return False


def regular_file(path):
    try:
        metadata = os.lstat(path)
    except OSError as error:
        fail("cannot inspect %s: %s" % (path, error))
    if not stat.S_ISREG(metadata.st_mode):
        fail("path is not a regular file: %s" % path)
    return metadata


def member_info(name, size):
    info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_STORED
    info.create_system = 3
    info.external_attr = (stat.S_IFREG | 0o644) << 16
    info.file_size = size
    return info


def main():
    if len(sys.argv) != 3:
        fail("usage: build_forager_port.py SOURCE_APK OUTPUT_PORT")

    source = os.path.realpath(sys.argv[1])
    output = os.path.abspath(sys.argv[2])
    stage_value = os.environ.get("NXEXTRACT_STAGE")
    if stage_value:
        stage = os.path.realpath(stage_value)
        if not inside(stage, source) or not inside(stage, output):
            fail("input and output must stay inside NXEXTRACT_STAGE")

    regular_file(source)
    parent = os.path.dirname(output)
    os.makedirs(parent, mode=0o755, exist_ok=True)
    temporary = output + ".partial"
    if os.path.lexists(temporary):
        fail("stale partial output exists")

    expected = tuple(SOURCE_MEMBERS) + tuple(sorted(INJECTED_MEMBERS))
    try:
        with zipfile.ZipFile(source, "r") as reader:
            source_names = reader.namelist()
            if len(source_names) != len(set(source_names)):
                fail("owner APK contains duplicate member names")
            for name in SOURCE_MEMBERS:
                info = reader.getinfo(name)
                expected_size, unused_digest = SOURCE_PROOFS[name]
                mode = (info.external_attr >> 16) & 0o170000
                if mode == stat.S_IFLNK or info.is_dir():
                    fail("unsafe source member: %s" % name)
                if info.file_size != expected_size:
                    fail("source member size mismatch: %s" % name)
                if info.file_size > MAX_MEMBER:
                    fail("source member is too large: %s" % name)

            with zipfile.ZipFile(
                    temporary, "w", compression=zipfile.ZIP_STORED,
                    allowZip64=True) as writer:
                for index, name in enumerate(SOURCE_MEMBERS):
                    source_info = reader.getinfo(name)
                    expected_size, expected_digest = SOURCE_PROOFS[name]
                    copied = 0
                    member_digest = hashlib.sha256()
                    print("NXEXTRACT_PROGRESS %d %d FORAGER %s" % (
                        index, len(expected), name))
                    with reader.open(source_info, "r") as source_stream:
                        with writer.open(
                                member_info(name, source_info.file_size),
                                "w") as output_stream:
                            while True:
                                block = source_stream.read(CHUNK)
                                if not block:
                                    break
                                copied += len(block)
                                member_digest.update(block)
                                output_stream.write(block)
                    if copied != expected_size or \
                            member_digest.hexdigest() != expected_digest:
                        fail("source member payload mismatch: %s" % name)
                for name in sorted(INJECTED_MEMBERS):
                    payload = INJECTED_MEMBERS[name]
                    writer.writestr(member_info(name, len(payload)), payload)

        with open(temporary, "rb") as stream:
            os.fsync(stream.fileno())
        os.replace(temporary, output)
        directory_fd = os.open(parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    except (KeyError, OSError, zipfile.BadZipFile) as error:
        try:
            if os.path.isfile(temporary):
                os.unlink(temporary)
        except OSError:
            pass
        fail("cannot build runtime archive: %s" % error)

    with zipfile.ZipFile(output, "r") as archive:
        if tuple(archive.namelist()) != expected:
            fail("generated archive inventory mismatch")
        if any(item.compress_type != zipfile.ZIP_STORED
               for item in archive.infolist()):
            fail("generated archive contains compressed members")
        broken = archive.testzip()
        if broken is not None:
            fail("generated archive CRC failed: %s" % broken)
    # The owner APK is only a transactional hook input. Keeping it inside the
    # committed runtime would duplicate 129 MiB and make the installed payload
    # depend on a file the loader never reads.
    if source != os.path.realpath(output):
        os.unlink(source)
        source_parent_fd = os.open(
            os.path.dirname(source), os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(source_parent_fd)
        finally:
            os.close(source_parent_fd)
    print("NXEXTRACT_PROGRESS %d %d FORAGER PRONTO" % (
        len(expected), len(expected)))


if __name__ == "__main__":
    main()
