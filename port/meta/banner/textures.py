#!/usr/bin/env python3
"""Draws the pictures the HOME Menu art is made of, all from code:

  icon.png    48 x 48   the HOME Menu and Homebrew Launcher icon: the website's mark, nine rusted
                        red squares on black (site/assets/favicon.svg), at its proportions
  icon-1024.png         the same icon large, to look at

Usage: textures.py OUT_DIR    (needs numpy and Pillow; the repo's .venv has both)
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

RNG = np.random.default_rng(2001)  # fixed, so the pictures only change when this file does


def value_noise(size, cells, rng):
    """Smooth noise that tiles: random values on a cells x cells grid, cubic-blended."""
    grid = rng.random((cells, cells))
    t = np.arange(size) * cells / size
    i0 = np.floor(t).astype(int)
    f = t - i0
    f = f * f * (3 - 2 * f)
    i1 = (i0 + 1) % cells
    a = grid[i0][:, i0] * (1 - f)[None, :] + grid[i0][:, i1] * f[None, :]
    b = grid[i1][:, i0] * (1 - f)[None, :] + grid[i1][:, i1] * f[None, :]
    return a * (1 - f)[:, None] + b * f[:, None]


def fractal(size, rng, octaves=5, base=4):
    out = np.zeros((size, size))
    amp, total = 1.0, 0.0
    for o in range(octaves):
        out += amp * value_noise(size, base * 2 ** o, rng)
        total += amp
        amp *= 0.5
    return out / total


def ramp(stops, x):
    """Colour at each value of x (0..1) from a list of (position, (r, g, b))."""
    x = np.clip(x, 0, 1)
    out = np.zeros(x.shape + (3,))
    for (p0, c0), (p1, c1) in zip(stops, stops[1:]):
        m = (x >= p0) & (x <= p1)
        t = ((x - p0) / max(p1 - p0, 1e-6))[m][:, None]
        out[m] = np.array(c0) * (1 - t) + np.array(c1) * t
    return out


def rust(size=128, coverage=0.47):
    """Painted red steel, rusted through in patches: the paint is the site's red (#b3121b),
    the rust a dark brown going orange where it has bloomed, with pitting and a few scratches."""
    rng = np.random.default_rng(RNG.integers(1 << 30))
    patches = fractal(size, rng, 5, 3)
    fine = fractal(size, rng, 4, 16)
    pits = fractal(size, rng, 2, 32)
    paint = np.array((0.70, 0.07, 0.09))
    rust_c = ramp([(0.0, (0.16, 0.05, 0.03)), (0.45, (0.36, 0.12, 0.05)), (0.75, (0.58, 0.25, 0.09)), (1.0, (0.72, 0.38, 0.14))], fine)
    cover = np.clip((patches - coverage) * 6.0, 0, 1)[..., None]           # 0 paint, 1 rust
    col = paint * (0.82 + 0.3 * (fine[..., None] - 0.5)) * (1 - cover) + rust_c * cover
    col *= 1 - 0.55 * np.clip((pits - 0.62) * 6, 0, 1)[..., None]     # dark pits
    img = Image.fromarray((np.clip(col, 0, 1) * 255).astype(np.uint8), "RGB")
    d = ImageDraw.Draw(img)
    for _ in range(5):                                                 # thin bright scratches
        x0, y0 = rng.integers(0, size, 2)
        dx, dy = rng.integers(-40, 40, 2)
        d.line([(x0, y0), (x0 + dx, y0 + dy // 4)], fill=(150, 70, 55), width=1)
    return img


def icon(size, rust_img):
    """favicon.svg at any size: a black rounded square holding a 3 x 3 grid of red squares
    (16 units: margin 2.5, square 3, gap 1), here in rusted metal with a lit top edge."""
    s = 16
    big = size * 8 if size < 256 else size
    k = big / s
    img = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([0, 0, big - 1, big - 1], radius=int(2 * k), fill=(5, 5, 5, 255))
    tex = rust_img.resize((big, big), Image.BICUBIC)
    for row in range(3):
        for col in range(3):
            x0, y0 = (2.5 + 4 * col) * k, (2.5 + 4 * row) * k
            x1, y1 = x0 + 3 * k, y0 + 3 * k
            box = [int(x0), int(y0), int(x1), int(y1)]
            tile = tex.crop(box).convert("RGBA")
            img.paste(tile, box[:2])
            e = max(1, int(0.12 * k))                                  # bevel: lit top edge, dark bottom edge
            over = Image.new("RGBA", img.size, (0, 0, 0, 0))
            od = ImageDraw.Draw(over)
            od.rectangle([box[0], box[1], box[2] - 1, box[1] + e - 1], fill=(255, 150, 130, 70))
            od.rectangle([box[0], box[3] - e, box[2] - 1, box[3] - 1], fill=(0, 0, 0, 110))
            img = Image.alpha_composite(img, over)
            d = ImageDraw.Draw(img)
    mask = Image.new("L", (big, big), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, big - 1, big - 1], radius=int(2 * k), fill=255)
    img.putalpha(mask)
    return img.resize((size, size), Image.LANCZOS)


def main(out):
    os.makedirs(out, exist_ok=True)
    RNG.integers(1 << 30, size=2)   # the seeds the banner's old rust and fog textures took
    # the icon is mostly paint, rust only at the edges of each square, so it still reads red at 48 px
    icon(48, rust(256, 0.6)).save(os.path.join(out, "icon.png"))
    icon(1024, rust(512, 0.6)).save(os.path.join(out, "icon-1024.png"))
    print("textures and icon in %s" % out)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
