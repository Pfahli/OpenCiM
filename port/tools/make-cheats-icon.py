# Draws the Cheats in Motion mod's button for the game's left bar (port/data/mods/cheats/textures/cheats.dds,
# cheats_se.dds, cheats_ho.dds): a magic wand with a gold star and sparkles, in the manner of the bar's own buttons
# (metro/ui/levelbar/*.dds: a glossy object standing on a faint reflection, 64x64 with transparent margins; the
# selected state with a soft dark glow behind it, the hover state drawn larger). Drawn from scratch here (no game
# art), at 4x and scaled down; written as uncompressed 32-bit DDS like the port's other icons.
#
#   py -3 tools\make-cheats-icon.py data\mods\cheats\textures [preview-dir]
import math
import os
import struct
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter

S = 4          # supersampling
N = 64 * S     # canvas


def star_points(cx, cy, r_out, r_in, n=5, rot=-math.pi / 2):
    pts = []
    for i in range(n * 2):
        r = r_out if i % 2 == 0 else r_in
        a = rot + i * math.pi / n
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def sparkle(draw, cx, cy, r, fill):
    # a four-pointed twinkle
    w = r * 0.28
    draw.polygon([(cx, cy - r), (cx + w, cy - w), (cx + r, cy), (cx + w, cy + w),
                  (cx, cy + r), (cx - w, cy + w), (cx - r, cy), (cx - w, cy - w)], fill=fill)


def wand(scale=1.0):
    """The object alone (no reflection), RGBA at N x N; scale about its centre for the hover state."""
    img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = (32 * S, 30 * S)

    k = 0.86 * scale  # the drawing below is a little taller than the bar's icons; it stands 3 pixels lower

    def p(x, y):  # icon pixels -> canvas, scaled about c
        return (c[0] + (x * S - c[0]) * k, c[1] + (y * S - c[1]) * k + 3 * S)

    # the wand: a thick black stick from lower left to the star, white tip at the lower end
    a, b = (13.0, 45.0), (39.0, 21.0)
    ang = math.atan2(b[1] - a[1], b[0] - a[0])
    nx, ny = -math.sin(ang), math.cos(ang)
    half = 2.6

    def quad(t0, t1, h):
        x0, y0 = a[0] + (b[0] - a[0]) * t0, a[1] + (b[1] - a[1]) * t0
        x1, y1 = a[0] + (b[0] - a[0]) * t1, a[1] + (b[1] - a[1]) * t1
        return [p(x0 + nx * h, y0 + ny * h), p(x1 + nx * h, y1 + ny * h),
                p(x1 - nx * h, y1 - ny * h), p(x0 - nx * h, y0 - ny * h)]

    d.polygon(quad(0.0, 1.0, half + 0.7), fill=(10, 10, 12, 255))      # dark outline
    d.polygon(quad(0.02, 0.98, half), fill=(28, 28, 32, 255))           # body
    d.polygon(quad(0.04, 0.96, half * 0.45), fill=(70, 70, 78, 255))    # gloss band
    d.polygon(quad(0.06, 0.6, half * 0.18), fill=(150, 150, 160, 255))  # highlight
    d.polygon(quad(0.0, 0.17, half), fill=(236, 236, 230, 255))         # white tip
    d.polygon(quad(0.01, 0.15, half * 0.35), fill=(255, 255, 255, 255))

    # the star at the upper end: orange rim, gold body, light upper half
    sc, sr = (43.0, 17.0), 12.5
    d.polygon([p(*q) for q in star_points(*sc, sr + 1.2, (sr + 1.2) * 0.45)], fill=(120, 60, 0, 255))
    d.polygon([p(*q) for q in star_points(*sc, sr, sr * 0.45)], fill=(232, 150, 20, 255))
    d.polygon([p(*q) for q in star_points(sc[0] - 0.6, sc[1] - 0.8, sr * 0.72, sr * 0.33)], fill=(255, 200, 60, 255))
    d.polygon([p(*q) for q in star_points(sc[0] - 1.2, sc[1] - 1.8, sr * 0.36, sr * 0.17)], fill=(255, 245, 190, 255))

    # sparkles around the star
    for (x, y, r) in [(25.0, 12.0, 4.2), (55.5, 31.0, 3.6), (31.0, 27.0, 2.4), (57.0, 9.0, 2.6)]:
        sparkle(d, *p(x, y), r * S * k, (255, 236, 150, 255))
        sparkle(d, *p(x, y), r * S * k * 0.45, (255, 255, 255, 255))
    return img


def with_reflection(obj, floor_y=48):
    """The object over a faint reflection below its floor line, as the bar's buttons stand."""
    fy = floor_y * S
    top = obj.crop((0, 0, N, fy))
    refl = top.transpose(Image.FLIP_TOP_BOTTOM)
    h = min(refl.height, N - fy)
    refl = refl.crop((0, 0, N, h))
    fade = Image.new("L", (N, h))
    fade.putdata([int(70 * max(0.0, 1.0 - y / (14.0 * S)) ** 1.6) for y in range(h) for _ in range(N)])
    ra = ImageChops.multiply(refl.getchannel("A"), fade)
    refl = refl.filter(ImageFilter.GaussianBlur(0.6 * S))
    refl.putalpha(ra)
    out = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    out.alpha_composite(refl, (0, fy))
    out.alpha_composite(obj)
    return out


def glow(img):
    """The selected state: a soft dark glow behind the icon (the bar's _se images)."""
    a = img.getchannel("A").filter(ImageFilter.MaxFilter(9)).filter(ImageFilter.GaussianBlur(5 * S))
    a = a.point(lambda v: int(v * 0.85))
    shadow = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    shadow.putalpha(a)
    shadow.alpha_composite(img)
    return shadow


def write_dds(img, path):
    img = img.resize((64, 64), Image.LANCZOS)
    w, h = img.size
    hdr = bytearray(128)
    hdr[0:4] = b"DDS "
    struct.pack_into("<IIIIIII", hdr, 4, 124, 0x1007 | 0x8, h, w, w * 4, 0, 1)
    struct.pack_into("<IIIIIIII", hdr, 76, 32, 0x41, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    struct.pack_into("<I", hdr, 108, 0x1000)
    r, g, b, a = img.split()
    data = Image.merge("RGBA", (b, g, r, a)).tobytes()   # BGRA
    with open(path, "wb") as f:
        f.write(hdr + data)
    return img


def main():
    out = sys.argv[1]
    prev = sys.argv[2] if len(sys.argv) > 2 else ""
    os.makedirs(out, exist_ok=True)
    normal = with_reflection(wand())
    states = {"": normal, "_se": glow(normal), "_ho": with_reflection(wand(1.14))}
    for suffix, img in states.items():
        small = write_dds(img, os.path.join(out, "cheats%s.dds" % suffix))
        if prev:
            os.makedirs(prev, exist_ok=True)
            small.save(os.path.join(prev, "cheats%s.png" % suffix))


if __name__ == "__main__":
    main()
