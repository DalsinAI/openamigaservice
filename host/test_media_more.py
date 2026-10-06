"""Checks media_decode.c's icons, font sheets and animated PNG and GIF.
Needs ImageMagick (icons are made with it; fonts are drawn by it) and ffmpeg.
MIT, Copyright (c) 2026 Dalsin Limited."""
import glob
import os
import struct
import subprocess
import tempfile
import media_decode as md

PROBE, DECODE, VOPEN, VFRAME, VCLOSE = 1, 2, 3, 4, 5


def hint(ext):
    return int.from_bytes(ext.upper().ljust(4).encode(), "big")


def call(op, data, ext="", room=24, arg=0, w=0, h=0):
    st, a, b, out = md.call(op, arg, [w, h, hint(ext) if ext else 0, 0], 2, [data, None, None, None], [0, room, 0, 0])
    return st, a, b, out[1]


with tempfile.TemporaryDirectory() as tmp:
    # An icon file with a 16x16 blue and a 48x48 green icon: the largest comes back.
    ico = os.path.join(tmp, "i.ico")
    subprocess.run(["convert", "-size", "16x16", "xc:blue", "(", "-size", "48x48", "xc:#00ff00", ")", ico], check=True)
    data = open(ico, "rb").read()
    st, w, h, info = call(PROBE, data)
    assert (st, w, h, info[4:8]) == (0, 48, 48, b"ICO "), (st, w, h, info[4:8])
    st, w, h, px = call(DECODE, data, room=48 * 48 * 4)
    assert (st, w, h, tuple(px[:4])) == (0, 48, 48, (255, 0, 255, 0)), (st, w, h, tuple(px[:4]))
    print("ico: the largest of two icons, 48x48")

    # A font comes back as ImageMagick's sample sheet.
    fonts = sorted(glob.glob("/usr/share/fonts/**/*.ttf", recursive=True))
    if fonts:
        st, w, h, info = call(PROBE, open(fonts[0], "rb").read(), "ttf")
        assert st == 0 and w >= 100 and h >= 100 and info[4:8] == b"TTF ", (st, w, h, info[4:8])
        print(f"ttf: sample sheet {w}x{h} of {os.path.basename(fonts[0])}")
    else:
        print("ttf: skipped, no TrueType font here")

    # Animated PNG and GIF open as a video without sound, frame by frame.
    for ext, args in (("apng", ["-plays", "0", "-f", "apng"]), ("gif", [])):
        path = os.path.join(tmp, "a." + ext)
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc=size=64x48:rate=5:duration=1"]
                       + args + [path], check=True)
        data = open(path, "rb").read()
        st, handle, fps, info = call(VOPEN, data, w=160, h=160)
        kind, fmt, flags, frames, w, h = struct.unpack(">6I", info)
        assert (st, kind, flags & 2, frames, w, h, fps) == (0, 2, 0, 5, 64, 48, 5000), (ext, st, flags, frames, w, h, fps)
        st, got, _, out = md.call(VFRAME, handle, [2, 1, 0, 0], 2, [None] * 4, [0, 64 * 48 * 3, 0, 0])
        assert st == 0 and got == 2 and sum(1 for b in out[1] if b) > 1000, (ext, st, got)
        md.call(VCLOSE, handle, [0, 0, 0, 0], 2, [None] * 4, [0, 0, 0, 0])
        # PROBE still answers a picture: the first frame.
        st, w, h, info = call(PROBE, data)
        assert (st, w, h) == (0, 64, 48), (ext, st, w, h)
        print(f"{ext}: 5 frames at 5 fps, 64x48, frame 2 drawn; PROBE gives the first frame")
