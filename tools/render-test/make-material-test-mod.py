#!/usr/bin/env python3
"""Writes a small test mod pack (.o2r) that gives a few of child Link's vanilla textures materials.

Usage: make-material-test-mod.py <output.o2r>

Everything in it is generated here (no game data): a bumpy normal map, a roughness-metal map and a
material manifest that points at vanilla texture paths. It exercises the material system in the
headless render tests: normal mapping, metalness, roughness and the eye shader.
"""
import io
import json
import math
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
    files = {
        base + "bumps.png": png_bytes(bump_normal_map(4)),
        base + "fine-bumps.png": png_bytes(bump_normal_map(16)),
        # glTF layout: G roughness, B metalness
        base + "polished-metal.png": png_bytes(solid((255, 70, 255, 255))),
        base + "leather.png": png_bytes(solid((255, 170, 0, 255))),
        # The shield's roughness-metal map is a texture resource so the test can rewrite it at runtime
        base + "shield-rm": rgba32_resource(solid((255, 70, 255, 255))),
    }
    link = "objects/object_link_child/"
    manifest = {
        "version": 1,
        "materials": [
            {"texture": link + "gLinkChildHylianShieldBackTex", "normal": base + "bumps.png",
             "roughnessMetal": base + "shield-rm"},
            {"texture": link + "gLinkChildDekuShieldBackTex", "normal": base + "bumps.png",
             "roughnessMetal": base + "polished-metal.png"},
            {"texture": link + "gLinkChildBootTex", "normal": base + "fine-bumps.png",
             "roughnessMetal": base + "leather.png"},
            {"texture": link + "gLinkChildBeltTex", "normal": base + "fine-bumps.png",
             "roughnessMetal": base + "leather.png"},
            {"texture": link + "gLinkChildHandTex", "skinMask": base + "polished-metal.png", "roughness": 0.55,
             "metalness": 0.0},
            {"texture": link + "gLinkChildEyesOpenTex", "shading": "eye"},
        ],
    }
    files["materials/render-test.json"] = json.dumps(manifest, indent=2).encode()
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, data in files.items():
            archive.writestr(name, data)


if __name__ == "__main__":
    main()
