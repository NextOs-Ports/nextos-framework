#!/usr/bin/env python3
"""nx-prompt-capture-proof/2 -- framebuffer capture + OCR + GLYPH gate (framework tool).

Evolution of nx-prompt-capture-proof/1 (E13 of the 03/09 audit): the /1 regex
approved "PRESS [10] TO BEGIN" on FP2 because the raw ordinal was a bare
number INSIDE the game's controller icon. /2 closes that hole:

  * NEGATIVE gate 1 (unchanged): no "Joystick Button 10" / "Button 1" /
    "Axis -2" / "Btn 3" anywhere in the recognised text;
  * NEGATIVE gate 2 (new): inside every declared PROMPT/GLYPH region, a
    standalone 1-2 digit token (optionally wrapped in [] {} () <>) is a raw
    ordinal -> FAIL `raw-ordinal-in-region`. Regions are OCR'd alone
    (psm 7, x4 upscale), where tesseract reads "PRESS {10} TO BEGIN";
  * POSITIVE glyph control (new): a capture can only PASS when at least one
    GLYPH region declares the expected token (A B X Y LB RB LT RT START
    SELECT) and that token is recognised there (OCR with a glyph whitelist)
    or matches a golden crop (normalised cross-correlation >= --golden-min).
    No glyph expectation, or expectation not met -> INCONCLUSIVE, never PASS;
  * POSITIVE word control (unchanged): declared words must be recognised.

Two modes:
  --target user@host  (device mode: trigger + /dev/fb0 capture, as /1)
  --offline name=file.png  (host mode: judge existing PNGs; fixtures/tests)

Capture spec (repeatable):
  --capture NAME:OFFSET_S[:word1,word2]
Region spec (repeatable, attaches to a capture):
  --region CAPTURE:REGION:x,y,w,h[:expect=TOKEN][:golden=path.png][:kind=prompt|glyph]
    x,y,w,h are fractions of the frame (0..1). kind defaults to glyph when
    expect= is given, prompt otherwise.

Self-test on the FP2 fixtures (negative):  --selftest DIR (DIR holds
fileselect.png/titleprompt.png/title.png from 03/09 10:09 on .101)

Receipt: <out>/receipt.json (sha256 of raw/png, OCR text, per-region verdicts).
The receipt is private evidence; the lock cites only its sha256 and verdict.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time

SCHEMA = "nx-prompt-capture-proof/2"
FORBIDDEN = re.compile(
    r"(?i)\bjoystick\b|\bbutton\s*\d|\baxis\s*[+-]?\s*\d|\baxis\s*\d\s*\([+-]\)|\bbtn\s*\d",
)
# a bare 1-2 digit token, optionally bracketed, standing alone between spaces
RAW_ORDINAL_TOKEN = re.compile(r"(?<![\w.])[\[\{\(<]?\s*(\d{1,2})\s*[\]\}\)>]?(?![\w.%])")
GLYPH_TOKENS = ("A", "B", "X", "Y", "LB", "RB", "LT", "RT", "START", "SELECT", "L1", "R1", "L2", "R2")
GLYPH_WHITELIST = "ABXYLRTSE0123456789"
V4_PATHS = (
    os.path.expanduser("~/.codex-worktrees/framework-v4-final"),
    os.path.expanduser("~/Área de trabalho/V4-QUATRO-ZIPS-FINAIS"),
)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def ssh(target, cmd, timeout=30):
    return subprocess.run(
        ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", target, cmd],
        capture_output=True, text=True, timeout=timeout,
    )


def tesseract(png, tessdata, psm, whitelist=None):
    env = dict(os.environ, TESSDATA_PREFIX=tessdata)
    cmd = ["tesseract", png, "-", "-l", "eng", "--psm", str(psm)]
    if whitelist:
        cmd += ["-c", "tessedit_char_whitelist=" + whitelist]
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    return " ".join(r.stdout.split())


def ocr_full(png, tessdata, work):
    from PIL import Image
    im = Image.open(png).convert("L")
    im = im.resize((im.width * 3, im.height * 3), Image.LANCZOS)
    up = os.path.join(work, os.path.basename(png) + ".ocr.png")
    im.save(up)
    texts = {"psm" + psm: tesseract(up, tessdata, psm) for psm in ("11", "6")}
    os.unlink(up)
    return texts


def crop_region(png, box, scale=4):
    from PIL import Image
    im = Image.open(png).convert("L")
    W, H = im.size
    x, y, w, h = box
    roi = im.crop((int(x * W), int(y * H), int((x + w) * W), int((y + h) * H)))
    return roi.resize((max(1, roi.width * scale), max(1, roi.height * scale)), Image.LANCZOS)


def ocr_region(png, box, tessdata, work, tag):
    """OCR one region four ways: plain, inverted, binarised bright-on-dark
    (the digit inside FP2's violet controller icon is white: threshold 200
    isolates it), each at psm 7 (one line) and the binarised one also at
    psm 6 (a column of icons)."""
    from PIL import Image, ImageOps
    roi = crop_region(png, box)
    up = os.path.join(work, tag + ".roi.png")
    out = {}
    roi.save(up)
    out["plain"] = tesseract(up, tessdata, 7)
    out["glyph_wl"] = tesseract(up, tessdata, 7, GLYPH_WHITELIST)
    ImageOps.invert(roi).save(up)
    out["inverted"] = tesseract(up, tessdata, 7)
    bw = roi.point(lambda v: 255 if v > 200 else 0)
    bw.save(up)
    out["bin7"] = tesseract(up, tessdata, 7)
    out["bin6"] = tesseract(up, tessdata, 6)
    out["bin_glyph_wl"] = tesseract(up, tessdata, 7, GLYPH_WHITELIST)
    os.unlink(up)
    return out


def golden_match(png, box, golden_path):
    """normalised cross-correlation of the region against a golden crop
    (both greyscale, golden resized to the region). 1.0 = identical."""
    from PIL import Image
    roi = crop_region(png, box, scale=1)
    g = Image.open(golden_path).convert("L").resize(roi.size, Image.LANCZOS)
    a = [p for p in roi.getdata()]
    b = [p for p in g.getdata()]
    n = len(a)
    ma, mb = sum(a) / n, sum(b) / n
    num = sum((x - ma) * (y - mb) for x, y in zip(a, b))
    da = sum((x - ma) ** 2 for x in a) ** 0.5
    db = sum((y - mb) ** 2 for y in b) ** 0.5
    return num / (da * db) if da and db else 0.0


def synth_prompt_frame(path, glyph):
    """Synthetic positive control: 'PRESS <glyph-in-icon> TO BEGIN' at 1280x720,
    white letter inside a violet rounded icon (FP2-like). Proves the gate can
    PASS when the expected glyph is really shown (never PASS by silence)."""
    from PIL import Image, ImageDraw, ImageFont
    im = Image.new("RGB", (1280, 720), (18, 40, 96))
    d = ImageDraw.Draw(im)
    try:
        big = ImageFont.load_default(size=34)
    except TypeError:  # old Pillow
        big = ImageFont.load_default()
    d.text((400, 545), "PRESS", fill=(255, 255, 255), font=big)
    d.rounded_rectangle((575, 536, 645, 586), radius=14, fill=(150, 99, 235))
    d.text((596, 545), glyph, fill=(255, 255, 255), font=big)
    d.text((690, 545), "TO BEGIN", fill=(255, 255, 255), font=big)
    im.save(path)


def judge_capture(png, tessdata, work, name, words, regions, golden_min):
    """Returns (verdict, detail). Verdict in PASS / FAIL / INCONCLUSIVE."""
    texts = ocr_full(png, tessdata, work)
    joined = " ".join(texts.values())
    low = joined.lower()
    forbidden = sorted(set(m.group(0) for m in FORBIDDEN.finditer(joined)))
    found = [wd for wd in words if wd.lower() in low]
    missing = [wd for wd in words if wd.lower() not in low]
    region_results = []
    fail_reasons = []
    glyph_declared = False
    glyph_met = False
    for reg in regions:
        tag = "%s.%s" % (name, reg["name"])
        r = ocr_region(png, reg["box"], tessdata, work, tag)
        raw = sorted(set(m.group(1) for k in ("plain", "inverted", "bin7", "bin6") for m in RAW_ORDINAL_TOKEN.finditer(r[k])))
        forb = sorted(set(m.group(0) for t in r.values() for m in FORBIDDEN.finditer(t)))
        item = {"name": reg["name"], "kind": reg["kind"], "box": reg["box"], "ocr": r,
                "raw_ordinal_tokens": raw, "forbidden_matches": forb}
        if raw:
            fail_reasons.append("raw-ordinal-in-region:%s:%s" % (reg["name"], ",".join(raw)))
        if forb:
            fail_reasons.append("forbidden-in-region:%s" % reg["name"])
        if reg.get("expect"):
            glyph_declared = True
            expect = reg["expect"].upper()
            tokens = set()
            for k in ("glyph_wl", "bin_glyph_wl", "plain", "bin7"):
                tokens |= set(re.findall(r"[A-Z0-9]+", r[k].upper()))
            by_ocr = expect in tokens
            by_golden = None
            if reg.get("golden"):
                try:
                    by_golden = golden_match(png, reg["box"], reg["golden"])
                except Exception as exc:  # golden unreadable = no evidence
                    by_golden = "error:%s" % exc
            met = by_ocr or (isinstance(by_golden, float) and by_golden >= golden_min)
            item.update({"expect": expect, "glyph_by_ocr": by_ocr, "glyph_golden_ncc": by_golden, "glyph_met": met})
            if met and not raw and not forb:
                glyph_met = True
        region_results.append(item)
    if forbidden:
        fail_reasons.append("forbidden-full-frame")
    if fail_reasons:
        verdict = "FAIL"
    elif words and not found:
        verdict = "INCONCLUSIVE"
        fail_reasons.append("positive-words-not-recognised")
    elif not glyph_declared:
        verdict = "INCONCLUSIVE"
        fail_reasons.append("no-glyph-expectation-declared (PASS impossible by silence)")
    elif not glyph_met:
        verdict = "INCONCLUSIVE"
        fail_reasons.append("expected-glyph-not-recognised")
    else:
        verdict = "PASS"
    detail = {
        "ocr": texts, "forbidden_matches": forbidden,
        "positive_control": {"expected": words, "found": found, "missing": missing},
        "regions": region_results, "reasons": fail_reasons, "result": verdict,
    }
    return verdict, detail


def parse_region(spec):
    parts = spec.split(":")
    if len(parts) < 3:
        sys.exit("bad --region %r" % spec)
    cap, name, box = parts[0], parts[1], [float(v) for v in parts[2].split(",")]
    if len(box) != 4:
        sys.exit("bad --region box %r" % spec)
    reg = {"capture": cap, "name": name, "box": box, "kind": "prompt"}
    for extra in parts[3:]:
        k, _, v = extra.partition("=")
        if k == "expect":
            reg["expect"] = v
            reg["kind"] = "glyph"
        elif k == "golden":
            reg["golden"] = v
        elif k == "kind":
            reg["kind"] = v
        else:
            sys.exit("bad --region option %r" % extra)
    return reg


def selftest(fixture_dir, tessdata, work, golden_min):
    """FP2 03/09 captures: /1 said PASS; /2 must FAIL them (E13)."""
    cases = [
        ("fileselect", ["PRESS", "BEGIN"],
         [{"name": "press-prompt", "box": [0.28, 0.74, 0.42, 0.08], "kind": "glyph", "expect": "A"}], "FAIL"),
        ("titleprompt", ["Dragon"],
         [{"name": "moveset-icons", "box": [0.60, 0.36, 0.08, 0.34], "kind": "prompt"},
          {"name": "moveset-icon-row1", "box": [0.60, 0.455, 0.08, 0.075], "kind": "glyph", "expect": "X"}], "FAIL"),
        ("title", ["unity"], [], "INCONCLUSIVE"),
    ]
    rc = 0
    for name, words, regions, want in cases:
        png = os.path.join(fixture_dir, name + ".png")
        if not os.path.exists(png):
            print("selftest: SKIP %s (missing fixture)" % name)
            continue
        verdict, detail = judge_capture(png, tessdata, work, name, words, regions, golden_min)
        ok = verdict == want
        print("selftest: %s %s want=%s got=%s reasons=%s" % ("ok  " if ok else "FAIL", name, want, verdict, detail["reasons"]))
        if not ok:
            rc = 1
    # positive control: a synthetic frame with the expected glyph must PASS,
    # and the same frame with a raw ordinal in the icon must FAIL
    for glyph, want in (("A", "PASS"), ("10", "FAIL"), ("B", "INCONCLUSIVE")):
        png = os.path.join(work, "synthetic-%s.png" % glyph)
        synth_prompt_frame(png, glyph)
        verdict, detail = judge_capture(png, tessdata, work, "synthetic-" + glyph, ["PRESS", "BEGIN"],
                                        [{"name": "press-prompt", "box": [0.28, 0.72, 0.42, 0.12], "kind": "glyph", "expect": "A"}], golden_min)
        ok = verdict == want
        print("selftest: %s synthetic icon '%s' expect=A want=%s got=%s reasons=%s" % ("ok  " if ok else "FAIL", glyph, want, verdict, detail["reasons"]))
        if not ok:
            rc = 1
    # /1 regex alone would have PASSed the two prompt frames: prove the hole is real
    for name in ("fileselect", "titleprompt"):
        png = os.path.join(fixture_dir, name + ".png")
        if os.path.exists(png):
            texts = ocr_full(png, tessdata, work)
            v1 = sorted(set(m.group(0) for m in FORBIDDEN.finditer(" ".join(texts.values()))))
            print("selftest: /1 forbidden regex on %s -> %s (the /1 hole)" % (name, v1 or "nothing = false PASS"))
    print("selftest:", "ALL OK" if rc == 0 else "FAIL")
    return rc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", help="user@host (device mode)")
    ap.add_argument("--geometry", help="WxH of /dev/fb0 (BGRA 32bpp)")
    ap.add_argument("--fbdev", default="/dev/fb0")
    ap.add_argument("--trigger-file", help="remote file to poll")
    ap.add_argument("--trigger-regex")
    ap.add_argument("--trigger-timeout", type=int, default=600)
    ap.add_argument("--capture", action="append", default=[],
                    help="name:offset_seconds[:word1,word2] (offset from trigger)")
    ap.add_argument("--region", action="append", default=[],
                    help="capture:name:x,y,w,h[:expect=TOKEN][:golden=path][:kind=prompt|glyph]")
    ap.add_argument("--offline", action="append", default=[], help="name=file.png (host mode)")
    ap.add_argument("--selftest", help="fixture dir with the FP2 03/09 PNGs")
    ap.add_argument("--golden-min", type=float, default=0.85)
    ap.add_argument("--tessdata", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--label", default="")
    ap.add_argument("--port", default="")
    ap.add_argument("--flow", default="")
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    for bad in V4_PATHS:
        if out.startswith(bad):
            sys.exit("refusing output under an immutable V4 path")
    os.makedirs(out, exist_ok=True)
    if a.selftest:
        return selftest(a.selftest, a.tessdata, out, a.golden_min)

    regions_by_capture = {}
    for spec in a.region:
        reg = parse_region(spec)
        regions_by_capture.setdefault(reg["capture"], []).append(reg)
    caps = []
    for spec in a.capture:
        parts = spec.split(":")
        words = parts[2].split(",") if len(parts) > 2 and parts[2] else []
        caps.append((parts[0], float(parts[1]) if len(parts) > 1 else 0.0, words))
    caps.sort(key=lambda c: c[1])

    results = []
    all_ok = True
    matched = None
    if a.offline:
        pngs = dict(item.split("=", 1) for item in a.offline)
        if not caps:
            caps = [(name, 0.0, []) for name in pngs]
        for name, off, words in caps:
            png = pngs.get(name)
            if not png or not os.path.exists(png):
                results.append({"name": name, "result": "INCONCLUSIVE", "reason": "offline png missing"})
                all_ok = False
                continue
            verdict, detail = judge_capture(png, a.tessdata, out, name, words, regions_by_capture.get(name, []), a.golden_min)
            detail.update({"name": name, "png": png, "png_sha256": sha256_file(png)})
            results.append(detail)
            all_ok = all_ok and verdict == "PASS"
            print("nx-prompt-capture-proof: %s %s reasons=%s" % (verdict, name, detail["reasons"]))
    else:
        if not (a.target and a.geometry and a.trigger_file and a.trigger_regex and caps):
            sys.exit("device mode needs --target --geometry --trigger-file --trigger-regex --capture")
        w, h = (int(x) for x in a.geometry.lower().split("x"))
        nbytes = w * h * 4
        t0 = time.time()
        while time.time() - t0 < a.trigger_timeout:
            r = ssh(a.target, "cat %s 2>/dev/null" % a.trigger_file)
            m = re.search(a.trigger_regex, r.stdout)
            if m:
                matched = m.group(0)
                break
            time.sleep(0.5)
        if matched is None:
            json.dump({"schema": SCHEMA, "result": "INCONCLUSIVE", "reason": "trigger not observed", "label": a.label},
                      open(os.path.join(out, "receipt.json"), "w"), indent=2)
            print("nx-prompt-capture-proof: INCONCLUSIVE (trigger not observed)")
            return 2
        t_trig = time.time()
        from PIL import Image
        for name, off, words in caps:
            delay = t_trig + off - time.time()
            if delay > 0:
                time.sleep(delay)
            raw = os.path.join(out, name + ".raw")
            ssh(a.target, "dd if=%s of=/tmp/nxcap.raw bs=%d count=1 2>/dev/null" % (a.fbdev, nbytes), timeout=60)
            subprocess.run(["scp", "-q", "-o", "BatchMode=yes", a.target + ":/tmp/nxcap.raw", raw], check=True, timeout=120)
            t_cap = time.time() - t_trig
            data = open(raw, "rb").read()
            if len(data) < nbytes:
                results.append({"name": name, "result": "INCONCLUSIVE", "reason": "short capture"})
                all_ok = False
                continue
            png = os.path.join(out, name + ".png")
            Image.frombytes("RGBA", (w, h), data[:nbytes], "raw", "BGRA").convert("RGB").save(png)
            verdict, detail = judge_capture(png, a.tessdata, out, name, words, regions_by_capture.get(name, []), a.golden_min)
            detail.update({"name": name, "offset_s": off, "captured_at_s": round(t_cap, 2),
                           "raw_sha256": sha256_file(raw), "png_sha256": sha256_file(png)})
            results.append(detail)
            all_ok = all_ok and verdict == "PASS"
            print("nx-prompt-capture-proof: %s %s reasons=%s" % (verdict, name, detail["reasons"]))
        ssh(a.target, "rm -f /tmp/nxcap.raw")
    receipt = {
        "schema": SCHEMA,
        "classification": "ON_DEVICE_PROMPT_CAPTURE_OCR_GLYPH" if not a.offline else "OFFLINE_PROMPT_OCR_GLYPH",
        "label": a.label, "port": a.port, "flow": a.flow,
        "geometry": a.geometry, "trigger": {"file": a.trigger_file, "regex": a.trigger_regex, "matched": matched},
        "forbidden_regex": FORBIDDEN.pattern, "raw_ordinal_token_regex": RAW_ORDINAL_TOKEN.pattern,
        "glyph_tokens": GLYPH_TOKENS, "golden_min": a.golden_min,
        "captures": results,
        "result": "PASS" if all_ok and results else "NOT_PASS",
    }
    with open(os.path.join(out, "receipt.json"), "w") as f:
        json.dump(receipt, f, indent=2, ensure_ascii=False)
    print("nx-prompt-capture-proof: %s receipt=%s" % (receipt["result"], os.path.join(out, "receipt.json")))
    return 0 if receipt["result"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
