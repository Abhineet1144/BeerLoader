"""QR code encoder (byte mode, error correction level M, versions 1-10) and a GTK widget that draws one.

Enough for Steam sign-in links (https://s.team/q/...); written from the QR specification (ISO/IEC 18004).
"""
import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
gi.require_version("Graphene", "1.0")
from gi.repository import Gdk, Graphene, Gtk  # noqa: E402

# Level M: (error-correction codewords per block, number of blocks) for versions 1-10.
_ECC_M = {1: (10, 1), 2: (16, 1), 3: (26, 1), 4: (18, 2), 5: (24, 2),
          6: (16, 4), 7: (18, 4), 8: (22, 4), 9: (22, 5), 10: (26, 5)}
_FORMAT_LEVEL_M = 0b00
MAX_VERSION = 10


def _raw_data_modules(version):
    """Modules left for data and error correction after all function patterns."""
    result = (16 * version + 128) * version + 64
    if version >= 2:
        count = version // 7 + 2
        result -= (25 * count - 10) * count - 55
        if version >= 7:
            result -= 36
    return result


def _data_capacity(version):
    ecc, blocks = _ECC_M[version]
    return _raw_data_modules(version) // 8 - ecc * blocks


def _alignment_positions(version):
    if version == 1:
        return []
    count = version // 7 + 2
    size = version * 4 + 17
    step = (version * 8 + count * 3 + 5) // (count * 4 - 4) * 2
    return [6] + sorted(size - 7 - i * step for i in range(count - 1))


# ---------------------------------------------------------------- Reed-Solomon over GF(256), polynomial 0x11D

def _gf_multiply(x, y):
    z = 0
    for i in range(7, -1, -1):
        z = (z << 1) ^ ((z >> 7) * 0x11D)
        z ^= ((y >> i) & 1) * x
    return z & 0xFF


def _rs_divisor(degree):
    result = [0] * (degree - 1) + [1]
    root = 1
    for _ in range(degree):
        for j in range(degree):
            result[j] = _gf_multiply(result[j], root)
            if j + 1 < degree:
                result[j] ^= result[j + 1]
        root = _gf_multiply(root, 0x02)
    return result


def _rs_remainder(data, divisor):
    result = [0] * len(divisor)
    for byte in data:
        factor = byte ^ result.pop(0)
        result.append(0)
        for i, coefficient in enumerate(divisor):
            result[i] ^= _gf_multiply(coefficient, factor)
    return result


# ---------------------------------------------------------------- encoding

def _codewords(text, version):
    payload = text.encode("utf-8")
    bits = []

    def put(value, length):
        bits.extend((value >> i) & 1 for i in range(length - 1, -1, -1))

    put(0b0100, 4)                                   # byte mode
    put(len(payload), 8 if version < 10 else 16)
    for byte in payload:
        put(byte, 8)
    capacity = _data_capacity(version) * 8
    put(0, min(4, capacity - len(bits)))             # terminator
    put(0, -len(bits) % 8)
    data = [int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8)]
    pad = 0xEC
    while len(data) < capacity // 8:
        data.append(pad)
        pad ^= 0xEC ^ 0x11

    # Split into blocks, add error correction, interleave.
    ecc, block_count = _ECC_M[version]
    raw = _raw_data_modules(version) // 8
    short_count = block_count - raw % block_count
    short_len = raw // block_count
    divisor = _rs_divisor(ecc)
    blocks, offset = [], 0
    for i in range(block_count):
        length = short_len - ecc + (0 if i < short_count else 1)
        chunk = data[offset:offset + length]
        offset += length
        blocks.append((chunk, _rs_remainder(chunk, divisor)))
    result = []
    for i in range(short_len - ecc + 1):
        result.extend(chunk[i] for chunk, _ in blocks if i < len(chunk))
    for i in range(ecc):
        result.extend(ec[i] for _, ec in blocks)
    return result


class _Matrix:
    def __init__(self, version):
        self.size = version * 4 + 17
        self.dark = [[False] * self.size for _ in range(self.size)]
        self.function = [[False] * self.size for _ in range(self.size)]

    def set(self, x, y, dark):
        self.dark[y][x] = dark
        self.function[y][x] = True


def _draw_function_patterns(m, version):
    size = m.size
    for i in range(size):
        m.set(6, i, i % 2 == 0)
        m.set(i, 6, i % 2 == 0)
    for cx, cy in ((3, 3), (size - 4, 3), (3, size - 4)):
        for dy in range(-4, 5):
            for dx in range(-4, 5):
                x, y = cx + dx, cy + dy
                if 0 <= x < size and 0 <= y < size:
                    m.set(x, y, max(abs(dx), abs(dy)) not in (2, 4))
    positions = _alignment_positions(version)
    last = len(positions) - 1
    for i, cx in enumerate(positions):
        for j, cy in enumerate(positions):
            if (i, j) in ((0, 0), (0, last), (last, 0)):
                continue                              # these overlap the finder patterns
            for dy in range(-2, 3):
                for dx in range(-2, 3):
                    m.set(cx + dx, cy + dy, max(abs(dx), abs(dy)) != 1)
    _draw_format_bits(m, 0)                           # reserve; rewritten once the mask is chosen
    if version >= 7:
        remainder = version
        for _ in range(12):
            remainder = (remainder << 1) ^ ((remainder >> 11) * 0x1F25)
        bits = version << 12 | remainder
        for i in range(18):
            dark = (bits >> i) & 1 == 1
            a, b = size - 11 + i % 3, i // 3
            m.set(a, b, dark)
            m.set(b, a, dark)


def _draw_format_bits(m, mask):
    data = _FORMAT_LEVEL_M << 3 | mask
    remainder = data
    for _ in range(10):
        remainder = (remainder << 1) ^ ((remainder >> 9) * 0x537)
    bits = (data << 10 | remainder) ^ 0x5412
    size = m.size

    def bit(i):
        return (bits >> i) & 1 == 1

    for i in range(6):
        m.set(8, i, bit(i))
    m.set(8, 7, bit(6))
    m.set(8, 8, bit(7))
    m.set(7, 8, bit(8))
    for i in range(9, 15):
        m.set(14 - i, 8, bit(i))
    for i in range(8):
        m.set(size - 1 - i, 8, bit(i))
    for i in range(8, 15):
        m.set(8, size - 15 + i, bit(i))
    m.set(8, size - 8, True)                          # the always-dark module


def _place_data(m, codewords):
    size = m.size
    index, total = 0, len(codewords) * 8
    right = size - 1
    while right >= 1:
        if right == 6:
            right = 5                                 # skip the vertical timing column
        upward = (right + 1) & 2 == 0
        for vertical in range(size):
            y = size - 1 - vertical if upward else vertical
            for x in (right, right - 1):
                if not m.function[y][x] and index < total:
                    m.dark[y][x] = (codewords[index >> 3] >> (7 - (index & 7))) & 1 == 1
                    index += 1
        right -= 2


_MASKS = [
    lambda x, y: (x + y) % 2 == 0,
    lambda x, y: y % 2 == 0,
    lambda x, y: x % 3 == 0,
    lambda x, y: (x + y) % 3 == 0,
    lambda x, y: (x // 3 + y // 2) % 2 == 0,
    lambda x, y: x * y % 2 + x * y % 3 == 0,
    lambda x, y: (x * y % 2 + x * y % 3) % 2 == 0,
    lambda x, y: ((x + y) % 2 + x * y % 3) % 2 == 0,
]


def _apply_mask(m, mask):
    test = _MASKS[mask]
    for y in range(m.size):
        for x in range(m.size):
            if not m.function[y][x] and test(x, y):
                m.dark[y][x] = not m.dark[y][x]


def _penalty(m):
    size, grid = m.size, m.dark
    score = 0
    lines = [row for row in grid] + [[grid[y][x] for y in range(size)] for x in range(size)]
    finder = [True, False, True, True, True, False, True]
    for line in lines:
        run, previous = 0, None
        for value in line:
            if value == previous:
                run += 1
            else:
                if run >= 5:
                    score += 3 + run - 5
                run, previous = 1, value
        if run >= 5:
            score += 3 + run - 5
        padded = [False] * 4 + line + [False] * 4
        for i in range(len(padded) - 6):
            if padded[i:i + 7] != finder:
                continue
            before, after = padded[max(0, i - 4):i], padded[i + 7:i + 11]
            if (len(before) == 4 and not any(before)) or (len(after) == 4 and not any(after)):
                score += 40
    for y in range(size - 1):
        for x in range(size - 1):
            if grid[y][x] == grid[y][x + 1] == grid[y + 1][x] == grid[y + 1][x + 1]:
                score += 3
    dark = sum(sum(row) for row in grid)
    total = size * size
    k = (abs(dark * 20 - total * 10) + total - 1) // total - 1
    return score + max(0, k) * 10


def encode(text):
    """Returns the QR code for text as a list of rows of booleans (True = dark), without the quiet zone."""
    length = len(text.encode("utf-8"))
    for version in range(1, MAX_VERSION + 1):
        header_bits = 4 + (8 if version < 10 else 16)
        if header_bits + length * 8 <= _data_capacity(version) * 8:
            break
    else:
        raise ValueError("text too long for a version %d QR code" % MAX_VERSION)
    codewords = _codewords(text, version)
    best = None
    for mask in range(8):
        m = _Matrix(version)
        _draw_function_patterns(m, version)
        _place_data(m, codewords)
        _apply_mask(m, mask)
        _draw_format_bits(m, mask)
        score = _penalty(m)
        if best is None or score < best[0]:
            best = (score, m)
    return best[1].dark


class QrCode(Gtk.Widget):
    """Draws a QR code, always dark on white (scanners expect that) with the 4-module quiet zone."""
    QUIET = 4

    def __init__(self, text="", size=240):
        super().__init__()
        self.set_size_request(size, size)
        self.set_halign(Gtk.Align.CENTER)
        self.set_valign(Gtk.Align.CENTER)
        self._modules = []
        self.set_text(text)

    def set_text(self, text):
        self._modules = encode(text) if text else []
        self.set_tooltip_text(text or None)
        self.queue_draw()

    def do_snapshot(self, snapshot):
        width, height = self.get_width(), self.get_height()
        side = min(width, height)
        if not self._modules or side <= 0:
            return
        count = len(self._modules) + 2 * self.QUIET
        module = side // count
        if module < 1:
            return
        drawn = module * count
        left, top = (width - drawn) // 2, (height - drawn) // 2
        white = Gdk.RGBA()
        white.parse("#ffffff")
        black = Gdk.RGBA()
        black.parse("#000000")
        snapshot.append_color(white, Graphene.Rect().init(left, top, drawn, drawn))
        for y, row in enumerate(self._modules):
            x = 0
            while x < len(row):
                if not row[x]:
                    x += 1
                    continue
                start = x
                while x < len(row) and row[x]:
                    x += 1
                snapshot.append_color(black, Graphene.Rect().init(
                    left + (start + self.QUIET) * module, top + (y + self.QUIET) * module,
                    (x - start) * module, module))
