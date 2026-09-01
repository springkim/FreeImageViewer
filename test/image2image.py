from pathlib import Path
from subprocess import run
from sys import argv
from PIL import Image
import pillow_jxl

p, f = Path(argv[1]), argv[2].lower()

formats = {
    "jxl":      ("jxl", "JXL"),
    "exr":      ("exr", None),
    "pnm":      ("pnm", "PPM"),
    "qoi":      ("qoi", "QOI"),
    "jpeg2000": ("jp2", "JPEG2000"),
    "heic":     ("heic", None),
    "tga":      ("tga", "TGA"),
    "pbm":      ("pbm", "PPM"),
    "pgm":      ("pgm", "PPM"),
    "ppm":      ("ppm", "PPM"),
    "pam":      ("pam", None),
}

e, n = formats[f]
o = p.with_suffix(f".{e}")

if f == "exr":
    x = "if(lte(val/maxval,0.04045),val/12.92,pow((val/maxval+0.055)/1.055,2.4)*maxval)"
    g = "format=rgb48le,lutrgb=" + ":".join(f"{c}='{x}'" for c in "rgb") + ",format=gbrpf32le"
    run(["ffmpeg", "-y", "-i", p, "-vf", g, "-frames:v", "1", o], check=True)

elif f == "heic":
    import pillow_heif

    with Image.open(p) as im:
        im = im.convert("RGB")
        pillow_heif.from_pillow(im).save(o)

elif f == "pam":
    # Pillow에서는 RGB PAM 출력 제어가 애매하므로 ffmpeg 사용
    run(["ffmpeg", "-y", "-i", p, "-frames:v", "1", o], check=True)

else:
    with Image.open(p) as im:
        if f == "pbm":
            im = im.convert("1")
        elif f == "pgm":
            im = im.convert("L")
        else:
            im = im.convert("RGB")

        im.save(o, n)