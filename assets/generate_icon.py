"""Generate USBGuard's multi-resolution Windows icon using Python's standard library."""
from __future__ import annotations

import argparse
import math
from pathlib import Path
import struct
import zlib

SIZES = (16, 24, 32, 48, 64, 128, 256)


def rounded_distance(x, y, left, top, right, bottom, radius):
    hx, hy = (right - left) / 2, (bottom - top) / 2
    cx, cy = (right + left) / 2, (bottom + top) / 2
    dx = abs(x - cx) - (hx - radius)
    dy = abs(y - cy) - (hy - radius)
    return math.hypot(max(dx, 0), max(dy, 0)) + min(max(dx, dy), 0) - radius


def mix(a, b, fraction):
    return tuple(round(x + (y - x) * fraction) for x, y in zip(a, b))


def color_at(name, x, y):
    fraction = min(1, max(0, (x + y - 12) / 165))
    if name == 'background':
        return mix((19, 61, 90), (8, 33, 57), fraction)
    if name == 'body':
        return mix((52, 213, 211), (19, 152, 181), fraction)
    return {
        'border': (53, 161, 193),
        'plug': (222, 241, 246),
        'slot': (66, 117, 137),
        'highlight': (160, 245, 232),
        'white': (255, 255, 255),
        'keyhole': (13, 54, 81),
    }[name]


def render(size):
    output = bytearray(size * size * 4)
    scale = 100.0 / size

    def composite(x, y, name, weight):
        if weight <= 0:
            return
        weight = min(1, weight)
        i = (y * size + x) * 4
        old_alpha = output[i + 3] / 255
        new_alpha = old_alpha + (1 - old_alpha) * weight
        source_color = color_at(name, (x + 0.5) * scale, (y + 0.5) * scale)
        if old_alpha > 0:
            for c in range(3):
                output[i + c] = round((output[i + c] * old_alpha * (1 - weight) + source_color[c] * weight) / new_alpha)
        else:
            for c in range(3):
                output[i + c] = source_color[c]
        output[i + 3] = round(new_alpha * 255)

    def draw_rect(bounds, radius, name):
        l, t, r, b = bounds
        for y in range(size):
            for x in range(size):
                px, py = (x + .5) * scale, (y + .5) * scale
                distance = rounded_distance(px, py, l, t, r, b, radius)
                composite(x, y, name, .5 - distance / scale)

    draw_rect((4, 4, 96, 96), 22, 'border')
    draw_rect((6.6, 6.6, 93.4, 93.4), 19.5, 'background')
    draw_rect((38, 18, 62, 45), 4.0, 'plug')
    draw_rect((42.5, 23.5, 47.8, 32.5), 1.3, 'slot')
    draw_rect((52.2, 23.5, 57.5, 32.5), 1.3, 'slot')
    draw_rect((23, 37, 77, 84), 13, 'body')
    draw_rect((34, 40, 67, 42.5), 1.2, 'highlight')
    draw_rect((40, 45, 60, 69), 9.9, 'white')
    draw_rect((45, 50, 55, 67), 5.0, 'body')
    draw_rect((37.5, 57, 62.5, 77), 4.5, 'white')
    draw_rect((46.8, 62.8, 53.2, 69.0), 3.1, 'keyhole')
    draw_rect((48.5, 67.4, 51.5, 72.0), 1.0, 'keyhole')
    return bytes(output)


def png_chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))


def encode_png(size, pixels):
    stride = size * 4
    scanlines = b''.join(b'\x00' + pixels[row * stride:(row + 1) * stride] for row in range(size))
    return (b'\x89PNG\r\n\x1a\n'
            + png_chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0))
            + png_chunk(b'IDAT', zlib.compress(scanlines, 9))
            + png_chunk(b'IEND', b''))



def encode_dib(size, pixels):
    """Encode an XP-compatible 32-bit icon DIB with an AND transparency mask."""
    stride = size * 4
    mask_stride = ((size + 31) // 32) * 4
    bitmap = bytearray()
    mask = bytearray()
    for y in range(size - 1, -1, -1):
        row = pixels[y * stride:(y + 1) * stride]
        for x in range(size):
            r, g, b, a = row[x * 4:x * 4 + 4]
            bitmap.extend((b, g, r, a))
        mask_row = bytearray(mask_stride)
        for x in range(size):
            if row[x * 4 + 3] < 32:
                mask_row[x // 8] |= 0x80 >> (x % 8)
        mask.extend(mask_row)
    header = struct.pack(
        '<IiiHHIIiiII', 40, size, size * 2, 1, 32, 0,
        len(bitmap) + len(mask), 0, 0, 0, 0
    )
    return header + bytes(bitmap) + bytes(mask)

def generate_icon(output_path):
    images = [encode_dib(size, render(size)) if size <= 48 else encode_png(size, render(size)) for size in SIZES]
    header = struct.pack('<HHH', 0, 1, len(SIZES))
    location = 6 + 16 * len(SIZES)
    directory = bytearray()
    for size, image in zip(SIZES, images):
        directory += struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(image), location)
        location += len(image)
    Path(output_path).parent.mkdir(parents=True, exist_ok=True)
    Path(output_path).write_bytes(header + bytes(directory) + b''.join(images))


if __name__ == '__main__':
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('destination', type=Path)
    args = cli.parse_args()
    generate_icon(args.destination)
