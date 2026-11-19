# Draws the button for pop-out views beside the minimap (port/data/regs/ui/minimap/popout.dds, popout_se.dds,
# popout_ho.dds): a window with an arrow leaving it, on the glossy light tile of the minimap's own buttons
# (metro/ui/minimap/data.dds: a rounded grey tile with a blue symbol, a faint reflection below; the selected state a
# dark tile with a glow, the hover state drawn larger). Drawn from scratch (no game art) at 4x and scaled down; written
# as uncompressed 32-bit DDS like the port's other icons.
#
#   py -3 tools\make-popout-icon.py data\regs\ui\minimap [preview-dir]
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter

S = 4          # supersampling
N = 64 * S     # canvas


def tile(scale=1.0, dark=False):
    """The tile and its symbol (no reflection), RGBA at N x N; scale about the tile's centre for the hover state."""
    img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx, cy = 31.0 * S, 31.0 * S

    def p(x, y):
        return (cx + (x * S - cx) * scale, cy + (y * S - cy) * scale)

    def box(x0, y0, x1, y1, r, fill):
        a, b = p(x0, y0), p(x1, y1)
        d.rounded_rectangle([a, b], radius=r * S * scale, fill=fill)

    # the tile: black rim, grey body lighter at the top (the game's buttons)
    box(15.5, 15.5, 46.5, 46.5, 3.5, (8, 8, 8, 255))
    if dark:
        box(16.8, 16.8, 45.2, 45.2, 2.6, (58, 58, 58, 255))
        box(17.5, 17.5, 44.5, 30.0, 2.2, (72, 72, 72, 255))
    else:
        box(16.8, 16.8, 45.2, 45.2, 2.6, (196, 196, 196, 255))
        box(17.5, 17.5, 44.5, 30.0, 2.2, (226, 226, 226, 255))
        box(18.0, 18.0, 44.0, 22.0, 1.8, (240, 240, 240, 255))

    rim = (6, 22, 60, 255) if not dark else (4, 14, 34, 255)
    blue = (30, 110, 220, 255) if not dark else (16, 56, 112, 255)
    light = (90, 175, 255, 255) if not dark else (36, 86, 150, 255)

    # the window: a frame with a title bar, lower left
    box(19.5, 26.0, 37.0, 42.5, 1.5, rim)
    box(20.8, 27.3, 35.7, 41.2, 0.9, blue)
    box(22.2, 31.2, 34.3, 39.8, 0.4, (235, 240, 248, 255) if not dark else (70, 74, 80, 255))
    box(21.5, 28.0, 34.9, 30.2, 0.4, light)

    # the arrow leaving it toward the upper right: shaft and head, dark outline under the blue
    def arrow(grow, fill):
        g = grow
        shaft = [p(29.0 - g, 33.5 + g), p(37.6 - g, 24.9 - g * 0.2), p(39.6 + g * 0.2, 26.9 + g), p(31.0 + g, 35.5 + g)]
        head = [p(34.0 - g * 1.6, 19.2 - g), p(43.6 + g, 19.2 - g), p(43.6 + g, 28.8 + g * 1.6)]
        d.polygon(shaft, fill=fill)
        d.polygon(head, fill=fill)

    arrow(1.3, rim)
    arrow(0.0, blue)
    d.polygon([p(35.4, 19.9), p(42.9, 19.9), p(40.4, 22.4)], fill=light)
    return img


def with_reflection(obj, floor_y=47):
    """The tile over a faint reflection below its floor line, as the minimap's buttons stand."""
    fy = floor_y * S
    top = obj.crop((0, 0, N, fy))
    refl = top.transpose(Image.FLIP_TOP_BOTTOM)
    h = min(refl.height, N - fy)
    refl = refl.crop((0, 0, N, h))
    fade = Image.new("L", (N, h))
    fade.putdata([int(60 * max(0.0, 1.0 - y / (12.0 * S)) ** 1.6) for y in range(h) for _ in range(N)])
    ra = ImageChops.multiply(refl.getchannel("A"), fade)
    refl = refl.filter(ImageFilter.GaussianBlur(0.6 * S))
    refl.putalpha(ra)
    out = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    out.alpha_composite(refl, (0, fy))
    out.alpha_composite(obj)
    return out


def glow(img):
    """The selected state: a soft light glow behind the dark tile (the minimap's _se images)."""
    a = img.getchannel("A").filter(ImageFilter.MaxFilter(9)).filter(ImageFilter.GaussianBlur(4 * S))
    a = a.point(lambda v: int(v * 0.55))
    halo = Image.new("RGBA", (N, N), (230, 230, 230, 0))
    halo.putalpha(a)
    halo.alpha_composite(img)
    return halo


def write_dds(img, path):
    img = img.resize((64, 64), Image.LANCZOS)
    w, h = img.size
    hdr = bytearray(128)
    hdr[0:4] = b"DDS "
    import struct
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
    states = {"": with_reflection(tile()), "_se": glow(with_reflection(tile(dark=True))),
              "_ho": with_reflection(tile(1.12))}
    for suffix, img in states.items():
        small = write_dds(img, os.path.join(out, "popout%s.dds" % suffix))
        if prev:
            os.makedirs(prev, exist_ok=True)
            small.save(os.path.join(prev, "popout%s.png" % suffix))


if __name__ == "__main__":
    main()
