from pathlib import Path
from subprocess import run
from sys import argv
from PIL import Image
import pillow_jxl

p, f = Path(argv[1]), argv[2].lower()
e, n = {"jxl": ("jxl", "JXL"), "exr": ("exr", None), "pnm": ("pnm", "PPM"), "qoi": ("qoi", "QOI"), "jpeg2000": ("jp2", "JPEG2000")}[f]
o = p.with_suffix(f".{e}")
if f == "exr":
    x = "if(lte(val/maxval,0.04045),val/12.92,pow((val/maxval+0.055)/1.055,2.4)*maxval)"
    g = "format=rgb48le,lutrgb=" + ":".join(f"{c}='{x}'" for c in "rgb") + ",format=gbrpf32le"
    run(["ffmpeg", "-y", "-i", p, "-vf", g, "-frames:v", "1", o], check=True)
else:
    with Image.open(p) as im:
        im.convert("RGB").save(o, n)
#  jxl, exr, pnm, qoi, jpeg2000
