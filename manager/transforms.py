"""Decoding and encoding of the transforms the loader's object scanner reports (see platform/beer_objects.h).

A decoded transform is (position, rotation, scale): position and scale are 3-tuples, rotation is
(yaw, pitch, roll) in degrees: yaw about Z, pitch about Y, roll about X, applied roll first (Z-up engines such
as RAGE and Unreal read yaw as the heading). encode() turns the edited values back into the bytes to write.
"""
import math
import struct

KIND_BYTES = {"mat4": 64, "mat34": 64, "mat4t": 64, "qts": 48, "qts64": 96}


def _norm(v):
    return math.sqrt(sum(x * x for x in v))


def _axes_from_euler(yaw, pitch, roll):
    """Rows: the images of the local X, Y and Z axes (row-vector convention, as stored by DirectX-style engines)."""
    cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
    cr, sr = math.cos(math.radians(roll)), math.sin(math.radians(roll))
    # Column-vector C = Rz(yaw) Ry(pitch) Rx(roll); the rows stored are C's columns.
    c = [[cy * cp, cy * sp * sr - sy * cr, cy * sp * cr + sy * sr],
         [sy * cp, sy * sp * sr + cy * cr, sy * sp * cr - cy * sr],
         [-sp, cp * sr, cp * cr]]
    return [[c[0][i], c[1][i], c[2][i]] for i in range(3)]


def _euler_from_axes(rows):
    """Inverse of _axes_from_euler for unit, orthogonal rows."""
    pitch = math.degrees(math.asin(max(-1.0, min(1.0, -rows[0][2]))))
    yaw = math.degrees(math.atan2(rows[0][1], rows[0][0]))
    roll = math.degrees(math.atan2(rows[1][2], rows[2][2]))
    return yaw, pitch, roll


def _axes_from_quat(x, y, z, w):
    """Rows (images of the local axes) of a unit quaternion's rotation."""
    return [[1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)],
            [2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)],
            [2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)]]


def _quat_from_axes(r):
    """Unit quaternion (x, y, z, w) for rows from _axes_from_quat."""
    m = [[r[j][i] for j in range(3)] for i in range(3)]   # column-vector matrix
    trace = m[0][0] + m[1][1] + m[2][2]
    if trace > 0:
        s = math.sqrt(trace + 1.0) * 2
        w, x, y, z = 0.25 * s, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s
    elif m[0][0] > m[1][1] and m[0][0] > m[2][2]:
        s = math.sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2
        w, x, y, z = (m[2][1] - m[1][2]) / s, 0.25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s
    elif m[1][1] > m[2][2]:
        s = math.sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2
        w, x, y, z = (m[0][2] - m[2][0]) / s, (m[0][1] + m[1][0]) / s, 0.25 * s, (m[1][2] + m[2][1]) / s
    else:
        s = math.sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2
        w, x, y, z = (m[1][0] - m[0][1]) / s, (m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25 * s
    n = math.sqrt(x * x + y * y + z * z + w * w) or 1.0
    return x / n, y / n, z / n, w / n


def position(kind, v):
    if kind in ("mat4", "mat34"):
        return tuple(v[12:15])
    if kind == "mat4t":
        return (v[3], v[7], v[11])
    return tuple(v[4:7])


def decode(kind, v):
    """(position, (yaw, pitch, roll), scale) from the value list the loader reports."""
    if kind in ("mat4", "mat34", "mat4t"):
        if kind == "mat4t":
            rows = [[v[c * 4 + r] for c in range(3)] for r in range(3)]
        else:
            rows = [v[0:3], v[4:7], v[8:11]]
        scale = tuple(_norm(r) or 1.0 for r in rows)
        unit = [[x / s for x in r] for r, s in zip(rows, scale)]
        return position(kind, v), _euler_from_axes(unit), scale
    rows = _axes_from_quat(*v[0:4])
    return tuple(v[4:7]), _euler_from_axes(rows), tuple(v[8:11])


def encode(kind, v, pos, rot, scale):
    """Bytes to write at the transform address (whole transform, unused lanes kept from v)."""
    v = list(v)
    rows = [[x * s for x in r] for r, s in zip(_axes_from_euler(*rot), scale)]
    if kind in ("mat4", "mat34"):
        for i in range(3):
            v[i * 4:i * 4 + 3] = rows[i]
        v[12:15] = pos
        return struct.pack("<16f", *v)
    if kind == "mat4t":
        for r in range(3):
            for c in range(3):
                v[c * 4 + r] = rows[r][c]
        v[3], v[7], v[11] = pos
        return struct.pack("<16f", *v)
    q = _quat_from_axes(_axes_from_euler(*rot))
    v[0:4] = q
    v[4:7] = pos
    v[8:11] = scale
    return struct.pack("<12d" if kind == "qts64" else "<12f", *v)


def position_offset(kind):
    """Byte offset and element format of the position inside the transform (for freezing it)."""
    return {"mat4": (48, "f32", 4), "mat34": (48, "f32", 4), "mat4t": (None, "f32", 4),
            "qts": (16, "f32", 4), "qts64": (32, "f64", 8)}[kind]


def position_addresses(kind, transform):
    """Addresses of the x, y, z position components and their memory type."""
    if kind == "mat4t":
        return [transform + 12, transform + 28, transform + 44], "f32"
    offset, fmt, step = position_offset(kind)
    return [transform + offset + i * step for i in range(3)], fmt
