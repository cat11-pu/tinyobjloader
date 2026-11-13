#!/usr/bin/env python3
"""Build a side-by-side render comparison from the PPM outputs and a
stats.json produced by vertex_merge_tool."""

import json
import sys

from PIL import Image, ImageDraw


def load_font(size: int):
    for candidate in (
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    ):
        try:
            from PIL import ImageFont

            return ImageFont.truetype(candidate, size)
        except OSError:
            continue
    return ImageFont.load_default()


def load_ppm(path: str) -> Image.Image:
    with open(path, "rb") as fp:
        header = fp.readline().strip()
        if header != b"P6":
            raise ValueError(f"not a binary PPM: {path}")
        line = fp.readline().strip()
        while line.startswith(b"#"):
            line = fp.readline().strip()
        width, height = (int(x) for x in line.split())
        max_value = int(fp.readline().strip())
        if max_value != 255:
            raise ValueError("only 8-bit PPM supported")
        data = fp.read(width * height * 3)
    return Image.frombytes("RGB", (width, height), data)


def main() -> None:
    out_dir = sys.argv[1]
    before = load_ppm(f"{out_dir}/before.ppm")
    after = load_ppm(f"{out_dir}/after.ppm")
    with open(f"{out_dir}/stats.json", encoding="utf-8") as fp:
        stats = json.load(fp)

    width, height = before.size
    top_band = 72
    bottom_band = 40
    gap = 12
    canvas = Image.new(
        "RGB",
        (width * 2 + gap * 3, height + top_band + bottom_band + gap * 3),
        (24, 24, 28),
    )
    draw = ImageDraw.Draw(canvas)
    font_title = load_font(22)
    font_body = load_font(17)

    x0, x1 = gap, gap * 2 + width
    y = top_band + gap
    canvas.paste(before, (x0, y))
    canvas.paste(after, (x1, y))

    title = (
        f"Vertex weld: {stats['vertex_count_before']} -> "
        f"{stats['vertex_count_after']} vertices "
        f"({stats['vertex_reduction_pct']:.2f}% fewer), "
        f"{stats['index_count_after']} indices, "
        f"{stats['triangle_count']} triangles"
    )
    timing = (
        f"parse {stats['parse_ms']:.3f} ms | weld "
        f"{stats['weld_ms']:.3f} ms "
        f"({stats['weld_over_parse_pct']:.2f}% of parse) | "
        f"pixel diffs: {stats['diff_pixels']}"
    )
    draw.text((gap, 8), title, fill=(245, 245, 250), font=font_title)
    draw.text((gap, 38), timing, fill=(150, 190, 255), font=font_body)

    draw.text((x0, y + height + 10),
              "Before: per-corner vertex soup (identity indices)",
              fill=(245, 245, 250), font=font_body)
    draw.text((x1, y + height + 10),
              "After: welded vertices, rebuilt index table",
              fill=(245, 245, 250), font=font_body)

    canvas.save(f"{out_dir}/render_compare.png")
    print(f"wrote {out_dir}/render_compare.png ({canvas.size[0]}x"
          f"{canvas.size[1]})")


if __name__ == "__main__":
    main()
