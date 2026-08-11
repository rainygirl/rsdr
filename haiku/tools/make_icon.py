# -*- coding: utf-8 -*-
"""Generate the front-facing silver radio HVIF in src/app.rdef."""
import io
import math
import os
import re


def coord(value):
    value = int(round(value)) + 32
    if not 0 <= value <= 127:
        raise ValueError("HVIF coordinate outside 0..64")
    return bytes([value])


def polygon(points):
    out = bytearray([0x0a, len(points)])  # closed, straight path
    for x, y in points:
        out += coord(x) + coord(y)
    return out


def rect(x0, y0, x1, y1, cut=0):
    return [(x0 + cut, y0), (x1 - cut, y0), (x1, y0 + cut),
            (x1, y1 - cut), (x1 - cut, y1), (x0 + cut, y1),
            (x0, y1 - cut), (x0, y0 + cut)]


def disc(cx, cy, radius, count=12):
    return [(cx + radius * math.cos(2 * math.pi * i / count),
             cy + radius * math.sin(2 * math.pi * i / count))
            for i in range(count)]


ORIGIN = (32.0, 10.0)
U = (26.0, 13.0)
V = (-26.0, 13.0)
DEPTH = 22.0


def top(u, v):
    return (ORIGIN[0] + u * U[0] + v * V[0],
            ORIGIN[1] + u * U[1] + v * V[1])


def left(u, down):
    base = top(0, 1)
    return (base[0] + u * U[0], base[1] + u * U[1] + down * DEPTH)


def right(v, down):
    base = top(1, 0)
    return (base[0] + v * V[0], base[1] + v * V[1] + down * DEPTH)


def project(projection, points):
    return [projection(a, b) for a, b in points]


def face_rect(projection, a0, b0, a1, b1):
    return project(projection, [(a0, b0), (a1, b0), (a1, b1), (a0, b1)])


def face_disc(projection, ca, cb, ra, rb, count=12):
    return project(projection,
                   [(ca + ra * math.cos(2 * math.pi * i / count),
                     cb + rb * math.sin(2 * math.pi * i / count))
                    for i in range(count)])


def hvif():
    styles = [
        (232, 237, 240, 255),    # bright silver top
        (172, 181, 187, 255),    # silver left face
        (112, 123, 131, 255),    # silver right face
        (38, 44, 48, 255),       # speaker panel
        (11, 18, 22, 255),       # grille holes
        (31, 91, 119, 255),      # blue display
        (104, 221, 255, 255),    # display line
        (244, 247, 248, 255),    # dial highlight
        (74, 83, 90, 255),       # dial shadow / antenna
		(43, 49, 54, 255),       # feet / outline accents
    ]
    groups = []

    def add(style, points):
        groups.append((style, points))

    # Antenna first, so the cabinet covers its root.
    root = top(0.18, 0.18)
    add(8, [(root[0] - 1, root[1]), (root[0] + 1, root[1] + 1),
            (7, 2), (5, 1)])

    # Standard BeOS diamond/isometric cabinet: two vertical faces and a
    # bright top. Silver metal and the digital controls distinguish it from
    # R WorldRadio's warm wooden set while preserving the family projection.
    add(1, [top(0, 1), top(1, 1), left(1, 1), left(0, 1)])
    add(2, [top(1, 0), top(1, 1), right(1, 1), right(0, 1)])
    add(0, [top(0, 0), top(1, 0), top(1, 1), top(0, 1)])

    # Recessed blue frequency display and two metallic tuning controls on
    # the left face.
    add(8, face_rect(left, 0.08, 0.13, 0.91, 0.43))
    add(5, face_rect(left, 0.13, 0.17, 0.86, 0.38))
    add(6, face_rect(left, 0.24, 0.25, 0.75, 0.29))
    add(8, face_disc(left, 0.29, 0.70, 0.16, 0.18, 14))
    add(7, face_disc(left, 0.29, 0.70, 0.12, 0.14, 14))
    add(8, face_disc(left, 0.69, 0.70, 0.12, 0.15, 14))
    add(7, face_disc(left, 0.69, 0.70, 0.08, 0.10, 12))

    # Dark speaker grille on the other flank, with silver horizontal slats.
    add(3, face_rect(right, 0.09, 0.12, 0.91, 0.88))
    for row in range(5):
        y = 0.18 + row * 0.14
        add(4, face_rect(right, 0.16, y, 0.84, y + 0.055))

    # A low handle on the top and two dark feet complete the silhouette.
    add(2, face_rect(top, 0.23, 0.40, 0.77, 0.60))
    add(9, face_rect(left, 0.08, 0.96, 0.23, 1.12))
    add(9, face_rect(right, 0.08, 0.96, 0.23, 1.12))

    out = bytearray(b"ncif")
    out.append(len(styles))
    for color in styles:
        out.append(1)
        out += bytes(color)
    out.append(len(groups))
    for _, points in groups:
        out += polygon(points)
    out.append(len(groups))
    for index, (style, _) in enumerate(groups):
        out += bytes([0x0a, style, 1, index, 0])
    return bytes(out)


def resource(data):
    value = "".join("%02X" % byte for byte in data)
    lines = ["resource vector_icon {"]
    lines += ['\t$"%s"' % value[i:i + 64] for i in range(0, len(value), 64)]
    lines.append("};")
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    path = os.path.join(root, "src", "app.rdef")
    text = io.open(path, encoding="utf-8").read()
    block = re.compile(r"^resource vector_icon \{.*?^\};\n", re.S | re.M)
    if len(block.findall(text)) != 1:
        raise SystemExit("expected one vector_icon resource")
    data = hvif()
    io.open(path, "w", encoding="utf-8").write(
        block.sub(lambda _: resource(data), text, count=1))
    print("wrote %d-byte HVIF" % len(data))
