#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Structural mutations of one nxbundle-v1 seed, for the launcher negatives.

Each mutation rewrites the ASCII header (and payload when required) so the
seed stays syntactically plausible.  The launcher must still refuse it.
"""

import hashlib
import sys


def split_bundle(path):
    data = open(path, "rb").read()
    end = data.index(b"\nEND\n") + 5
    header = data[:end].decode("ascii")
    return header, data[end:]


def write_bundle(path, header, payload):
    with open(path, "wb") as stream:
        stream.write(header.encode("ascii"))
        stream.write(payload)


def records(header):
    return [line for line in header.split("\n") if line.startswith("M\t")]


def main(argv):
    path = argv[1]
    action = argv[2]
    header, payload = split_bundle(path)
    lines = header.split("\n")
    member_lines = records(header)
    if action == "rename":
        target = argv[3]
        first = member_lines[0]
        fields = first.split("\t")
        fields[5] = target
        header = header.replace(first, "\t".join(fields), 1)
    elif action == "duplicate":
        first = member_lines[0]
        fields = first.split("\t")
        second = member_lines[1].split("\t")
        second[5] = fields[5]
        header = header.replace(member_lines[1], "\t".join(second), 1)
    elif action == "casefold":
        # Distinct in the header, the same file on exFAT.
        first = member_lines[0].split("\t")
        second = member_lines[1].split("\t")
        head, _, tail = first[5].rpartition("/")
        second[5] = "%s/%s" % (head, tail.upper()) if head else tail.upper()
        if second[5] == first[5]:
            raise SystemExit("casefold mutation produced no case change")
        header = header.replace(member_lines[1], "\t".join(second), 1)
    elif action == "extra":
        # A member nobody declared: the closure validator must reject it.
        blob = b"stray\n"
        offset = sum(int(line.split("\t")[3]) for line in member_lines)
        record = "M\t0644\t%s\t%d\t%d\tfiles/runtime/stray.bin" % (
            hashlib.sha256(blob).hexdigest(), len(blob), offset)
        lines = header.split("\n")
        end_index = lines.index("END")
        lines.insert(end_index, record)
        for index, line in enumerate(lines):
            if line.startswith("members "):
                lines[index] = "members %d" % (len(member_lines) + 1)
        header = "\n".join(lines)
        payload = payload + blob
    elif action == "drop":
        victim = member_lines[-1]
        size = int(victim.split("\t")[3])
        offset = int(victim.split("\t")[4])
        lines = [line for line in header.split("\n") if line != victim]
        for index, line in enumerate(lines):
            if line.startswith("members "):
                lines[index] = "members %d" % (len(member_lines) - 1)
        header = "\n".join(lines)
        payload = payload[:offset] + payload[offset + size:]
    elif action == "miscount":
        for index, line in enumerate(lines):
            if line.startswith("members "):
                lines[index] = "members %d" % (len(member_lines) + 3)
        header = "\n".join(lines)
    elif action == "shift-offset":
        victim = member_lines[-1]
        fields = victim.split("\t")
        fields[4] = str(int(fields[4]) + 1)
        header = header.replace(victim, "\t".join(fields), 1)
    elif action == "mode":
        victim = member_lines[0]
        fields = victim.split("\t")
        fields[1] = "0777"
        header = header.replace(victim, "\t".join(fields), 1)
    else:
        raise SystemExit("unknown mutation: %s" % action)
    write_bundle(path, header, payload)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
