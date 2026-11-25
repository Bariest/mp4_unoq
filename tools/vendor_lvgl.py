#!/usr/bin/env python3
"""vendor_lvgl.py -- copy the part of LVGL this sketch uses into sketch/src/lvgl.

    python3 tools/vendor_lvgl.py <lvgl folder of the ESP build (firmware-dev/lvgl)>

The Arduino build compiles every .c under sketch/src, and the UNO Q builds on
its own small Linux, so only the files that compile to something with
sketch/src/lv_conf.h are copied: each .c is compiled once here (host gcc) and
kept if its object has any symbol; then the headers those files and lvgl.h
include. lv_conf.h sits at sketch/src/, where LVGL looks for it
("../../lv_conf.h" from lvgl/src)."""
import os, re, shutil, subprocess, sys, tempfile

APP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DST = os.path.join(APP, "sketch", "src", "lvgl")
CONF_DIR = os.path.join(APP, "sketch", "src")
SKIP_DIRS = ("src/drivers", "src/libs/thorvg", "src/libs/ffmpeg", "src/libs/freetype", "src/libs/gstreamer",
             "src/libs/vg_lite_driver", "src/libs/nanovg", "src/libs/FT800-FT813", "src/draw/nxp", "src/draw/vg_lite",
             "src/draw/renesas", "src/draw/sdl", "src/draw/opengles", "src/draw/eve", "src/draw/nema_gfx",
             "src/draw/espressif", "src/draw/nanovg", "src/draw/dma2d", "src/debugging/test", "src/others/xml",
             "src/libs/gltf", "src/libs/rlottie", "src/libs/libwebp", "src/libs/libpng", "src/libs/libjpeg_turbo")


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    src = os.path.abspath(sys.argv[1])
    if not os.path.isfile(os.path.join(src, "lvgl.h")):
        raise SystemExit("not an LVGL folder: " + src)
    # a scratch tree with the same layout, so "../../lv_conf.h" resolves
    tmp = tempfile.mkdtemp()
    shutil.copytree(src, os.path.join(tmp, "lvgl"), ignore=shutil.ignore_patterns("*.md", "*.rst", "docs", "examples", "demos", "tests"))
    shutil.copy(os.path.join(CONF_DIR, "lv_conf.h"), tmp)
    root = os.path.join(tmp, "lvgl")
    keep_c, deps = [], set()
    flags = ["-std=gnu11", "-O0", "-w", "-I" + root]
    for dp, dn, fn in os.walk(os.path.join(root, "src")):
        rel = os.path.relpath(dp, root).replace(os.sep, "/")
        if any(rel == s or rel.startswith(s + "/") for s in SKIP_DIRS):
            dn[:] = []
            continue
        for f in fn:
            if not f.endswith(".c"):
                continue
            p = os.path.join(dp, f)
            obj = os.path.join(tmp, "t.o")
            r = subprocess.run(["gcc"] + flags + ["-c", p, "-o", obj, "-MMD", "-MF", obj + ".d"], capture_output=True, text=True)
            if r.returncode != 0:
                print("  (does not compile here, left out: %s)" % os.path.relpath(p, root))
                continue
            syms = subprocess.run(["nm", "--defined-only", obj], capture_output=True, text=True).stdout.strip()
            if not syms:
                continue
            keep_c.append(os.path.relpath(p, root))
            deps.update(os.path.realpath(d) for d in parse_deps(obj + ".d"))
    # lvgl.h and the headers the face includes directly
    for h in ("lvgl.h", "src/libs/gif/AnimatedGIF.h", "src/misc/cache/lv_cache.h"):
        hdr = os.path.join(root, h)
        stub = os.path.join(tmp, "h.c")
        with open(stub, "w") as f:
            f.write('#include "%s"\n' % hdr)
        r = subprocess.run(["gcc"] + flags + ["-c", stub, "-o", os.path.join(tmp, "h.o"), "-MMD", "-MF", stub + ".d"],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise SystemExit(r.stderr)
        deps.update(os.path.realpath(d) for d in parse_deps(stub + ".d"))
    rroot = os.path.realpath(root)
    hdrs = sorted(os.path.relpath(d, rroot) for d in deps if d.startswith(rroot + os.sep) and not d.endswith(".c"))
    if os.path.isdir(DST):
        shutil.rmtree(DST)
    n = 0
    for rel in keep_c + hdrs:
        out = os.path.join(DST, rel)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        shutil.copy(os.path.join(root, rel), out)
        n += os.path.getsize(out)
    for lic in ("LICENCE.txt",):
        if os.path.isfile(os.path.join(src, lic)):
            shutil.copy(os.path.join(src, lic), DST)
    with open(os.path.join(DST, "VENDORED.txt"), "w") as f:
        f.write("LVGL %s, copied by tools/vendor_lvgl.py: %d .c, %d .h\n" % (version(root), len(keep_c), len(hdrs)))
    print("vendored %d .c + %d .h (%d KB) into %s" % (len(keep_c), len(hdrs), n // 1024, os.path.relpath(DST, APP)))
    shutil.rmtree(tmp)


def parse_deps(dfile):
    with open(dfile) as f:
        text = f.read().replace("\\\n", " ")
    out = []
    for line in text.splitlines():
        if ":" in line:
            out += line.split(":", 1)[1].split()
    return out


def version(root):
    t = open(os.path.join(root, "lv_version.h")).read()
    v = [re.search(r"#define LVGL_VERSION_%s (\d+)" % k, t).group(1) for k in ("MAJOR", "MINOR", "PATCH")]
    return ".".join(v)


if __name__ == "__main__":
    main()
