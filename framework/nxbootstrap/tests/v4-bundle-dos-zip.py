#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Write a personal ZIP the way a DOS/Windows graphical archiver would.

Members are stored uncompressed, in reverse order, with a fixed far-future
timestamp, no Unix mode at all, DOS attributes set and a foreign extra field.
None of that may change what the launcher authenticates.
"""

import os
import sys
import zipfile


def main(argv):
    root, target, _launcher_name, port_id = argv[1:5]
    members = []
    for base, _dirs, files in os.walk(root):
        for name in files:
            path = os.path.join(base, name)
            members.append((os.path.relpath(path, root), path))
    members.sort(reverse=True)
    with zipfile.ZipFile(target, "w", zipfile.ZIP_STORED) as archive:
        archive.writestr(
            zipfile.ZipInfo(port_id + "/", (2107, 1, 1, 0, 0, 0)), b"")
        for relative, path in members:
            info = zipfile.ZipInfo(
                relative.replace(os.sep, "/"), (2107, 1, 1, 0, 0, 0))
            info.create_system = 0
            info.external_attr = 0x20
            info.extra = b"\xef\xbe\x04\x00spam"
            with open(path, "rb") as stream:
                archive.writestr(info, stream.read())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
