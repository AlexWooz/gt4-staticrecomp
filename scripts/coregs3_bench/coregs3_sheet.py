#!/usr/bin/env python3
"""TASK-090 CoreGS 3.0: milestone sheets (docs/COREGS3-PLAN.md section 8.2 item 4).

Composes a horizontal strip of images with a caption under each one and a title line, so a milestone picture carries the
scene, the VBlank and the build identity next to it. Test tool only: no game data, no Vulkan, nothing linked into the game.

    coregs3_sheet.py <out.png> --title "..." <image> <caption> [<image> <caption> ...]
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont


def load(path, height):
    image = Image.open(path).convert("RGB")
    scale = height / image.height
    return image.resize((max(1, round(image.width * scale)), height), Image.LANCZOS)


def main():
    argv = sys.argv[1:]
    if not argv:
        print(__doc__)
        raise SystemExit(2)
    out = argv.pop(0)
    title = ""
    height = 448
    background = "#101014"
    pairs = []
    index = 0
    while index < len(argv):
        argument = argv[index]
        if argument == "--title":
            title, index = argv[index + 1], index + 2
        elif argument == "--height":
            height, index = int(argv[index + 1]), index + 2
        elif argument == "--background":
            background, index = argv[index + 1], index + 2
        else:
            pairs.append(argument)
            index += 1
    if len(pairs) % 2 != 0 or not pairs:
        print(__doc__)
        raise SystemExit(2)
    images = []
    for index in range(0, len(pairs), 2):
        path, caption = pairs[index], pairs[index + 1]
        images.append((load(path, height), caption, os.path.basename(path)))

    try:
        font = ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf", 15)
        title_font = ImageFont.truetype("C:/Windows/Fonts/segoeuib.ttf", 18)
    except OSError:
        font = ImageFont.load_default()
        title_font = font

    gap = 12
    caption_height = 44
    title_height = 34 if title else 8
    width = sum(image.width for image, _, _ in images) + gap * (len(images) + 1)
    sheet_height = title_height + height + caption_height + gap
    sheet = Image.new("RGB", (width, sheet_height), background)
    draw = ImageDraw.Draw(sheet)
    if title:
        draw.text((gap, 8), title, fill="#F2F2F2", font=title_font)

    x = gap
    for image, caption, name in images:
        sheet.paste(image, (x, title_height))
        draw.text((x, title_height + height + 4), caption, fill="#E8E8E8", font=font)
        draw.text((x, title_height + height + 22), name, fill="#8A8A92", font=font)
        x += image.width + gap
    sheet.save(out)
    print(f"sheet {out}: {sheet.width}x{sheet.height}, {len(images)} panels")


if __name__ == "__main__":
    main()
