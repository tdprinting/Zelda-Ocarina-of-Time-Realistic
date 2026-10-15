#!/usr/bin/env python3
"""Writes a small test mod pack (.o2r) that gives a few of child Link's vanilla textures materials.

Usage: make-material-test-mod.py <output.o2r>

Everything in it is generated here (no game data): a bumpy normal map, a roughness-metal map and a
material manifest that points at vanilla texture paths. It exercises the material system in the
headless render tests: normal mapping, metalness, roughness and runtime map refreshes. Eye shading needs
no test pack: soh.o2r tags the vanilla eye textures itself (materials/00-vanilla-eyes.json).
"""
import io
import json
import math
import random
import struct
import sys
import zipfile

from PIL import Image

SIZE = 128


def png_bytes(image):
    out = io.BytesIO()
    image.save(out, format="PNG")
    return out.getvalue()


def bump_normal_map(cells):
    """Hemispherical bumps on a grid, OpenGL convention (+Y up the image)."""
    image = Image.new("RGBA", (SIZE, SIZE))
    pixels = image.load()
    cell = SIZE / cells
    for y in range(SIZE):
        for x in range(SIZE):
            cx = (x % cell) / cell * 2 - 1
            cy = (y % cell) / cell * 2 - 1
            r2 = cx * cx + cy * cy
            if r2 < 0.8:
                nx, ny = cx * 0.9, -cy * 0.9  # image Y points down, map +Y points up
            else:
                nx, ny = 0.0, 0.0
            nz = math.sqrt(max(0.0, 1 - nx * nx - ny * ny))
            pixels[x, y] = (int((nx * 0.5 + 0.5) * 255), int((ny * 0.5 + 0.5) * 255), int((nz * 0.5 + 0.5) * 255), 255)
    return image


def hammered_normal_map(dimples, seed=7):
    """Overlapping shallow dimples at random spots, like hammered metal (tiles seamlessly)."""
    rng = random.Random(seed)
    height = [[0.0] * SIZE for _ in range(SIZE)]
    for _ in range(dimples):
        cx, cy, r = rng.uniform(0, SIZE), rng.uniform(0, SIZE), rng.uniform(5, 11)
        for y in range(int(cy - r) - 1, int(cy + r) + 2):
            for x in range(int(cx - r) - 1, int(cx + r) + 2):
                d2 = ((x - cx) ** 2 + (y - cy) ** 2) / (r * r)
                if d2 < 1:
                    height[y % SIZE][x % SIZE] -= (1 - d2) ** 2
    image = Image.new("RGBA", (SIZE, SIZE))
    pixels = image.load()
    for y in range(SIZE):
        for x in range(SIZE):
            dx = (height[y][(x + 1) % SIZE] - height[y][x - 1]) * 0.6
            dy = (height[(y + 1) % SIZE][x] - height[y - 1][x]) * 0.6
            nx, ny, nz = -dx, dy, 1.0  # image Y points down, map +Y points up
            length = math.sqrt(nx * nx + ny * ny + nz * nz)
            pixels[x, y] = tuple(int((c / length * 0.5 + 0.5) * 255) for c in (nx, ny, nz)) + (255,)
    return image


def shield_wood(u, v):
    """True on the wooden middle of Hylian Shield's back texture: an ellipse against the left (mirrored)
    edge, inside the steel rim. u and v are 0..1 across the texture."""
    return (u / 0.76) ** 2 + ((v - 0.47) / 0.40) ** 2 < 1.0


def shield_maps():
    """Roughness-metal and normal maps for the back of Hylian Shield: hammered steel rim, matte wood."""
    width, height = SIZE, SIZE * 2  # the base texture is 16x32
    hammered = hammered_normal_map(60).load()
    rm = Image.new("RGBA", (width, height))
    normal = Image.new("RGBA", (width, height))
    for y in range(height):
        for x in range(width):
            if shield_wood((x + 0.5) / width, (y + 0.5) / height):
                rm.putpixel((x, y), (255, 190, 0, 255))  # roughness 0.75, not metal
                normal.putpixel((x, y), (128, 128, 255, 255))
            else:
                rm.putpixel((x, y), (255, 90, 255, 255))  # roughness 0.35, metal
                normal.putpixel((x, y), hammered[x, y % SIZE])
    return rm, normal


def solid(rgba):
    return Image.new("RGBA", (4, 4), rgba)


def rgba32_resource(image):
    """A libultraship binary texture resource (RGBA32), as mods ship maps they rewrite at runtime."""
    header = bytearray(0x40)
    header[4:8] = b"XETO"  # resource type OTEX, little endian
    header[12:20] = b"\xef\xbe\xad\xde" * 2
    data = image.convert("RGBA").tobytes()
    return bytes(header) + struct.pack("<IIII", 1, image.width, image.height, len(data)) + data


def main():
    out_path = sys.argv[1]
    base = "textures/render-test/"
    shield_rm, shield_normal = shield_maps()
    files = {
        base + "bumps.png": png_bytes(bump_normal_map(4)),
        base + "fine-bumps.png": png_bytes(bump_normal_map(16)),
        # glTF layout: G roughness, B metalness
        base + "polished-metal.png": png_bytes(solid((255, 70, 255, 255))),
        base + "leather.png": png_bytes(solid((255, 170, 0, 255))),
        # The shield's roughness-metal map is a texture resource so the test can rewrite it at runtime
        base + "shield-rm": rgba32_resource(shield_rm),
        base + "shield-normal.png": png_bytes(shield_normal),
    }
    link = "objects/object_link_child/"
    manifest = {
        "version": 1,
        "materials": [
            {"texture": link + "gLinkChildHylianShieldBackTex", "normal": base + "shield-normal.png",
             "roughnessMetal": base + "shield-rm", "normalStrength": 0.6},
            {"texture": link + "gLinkChildDekuShieldBackTex", "normal": base + "bumps.png",
             "roughnessMetal": base + "polished-metal.png"},
            {"texture": link + "gLinkChildBootTex", "normal": base + "fine-bumps.png",
             "roughnessMetal": base + "leather.png"},
            {"texture": link + "gLinkChildBeltTex", "normal": base + "fine-bumps.png",
             "roughnessMetal": base + "leather.png"},
            {"texture": link + "gLinkChildHandTex", "skinMask": base + "polished-metal.png", "roughness": 0.55,
             "metalness": 0.0},
        ],
    }
    files["materials/render-test.json"] = json.dumps(manifest, indent=2).encode()
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, data in files.items():
            archive.writestr(name, data)


if __name__ == "__main__":
    main()
