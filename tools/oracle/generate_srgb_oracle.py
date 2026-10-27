"""Generates the high-precision reference data for the conversion tests.

Independent oracle (decision T4): IEC 61966-2-1 formulas evaluated with Python's
decimal module at 60 significant digits, then rounded exactly to IEEE binary32.
Branches that are rational are evaluated exactly with fractions.Fraction. Any
rounding decision closer than MARGIN to a boundary aborts generation instead of
guessing; sampled cases that are too close to call are skipped and counted.

Writes <dir>/srgb_oracle_data.h (sRGB u8 <-> float, Milestone 1) and
<dir>/pixel_format_oracle_data.h (u16, luminance and gray cases, Milestone 2).

Usage: python tools/oracle/generate_srgb_oracle.py tests/data
"""
import struct
import sys
from decimal import Decimal, getcontext
from fractions import Fraction

getcontext().prec = 60
MARGIN = Fraction(1, 10**45)  # relative; far below binary32 resolution, far above decimal error

D = Decimal
LINEAR_LIMIT = Fraction(31308, 10**7)  # 0.0031308: OETF linear segment ends here
ENCODED_LIMIT = Fraction(4045, 10**5)  # 0.04045: EOTF linear segment ends here


def f32_value(bits: int) -> Fraction:
    return Fraction(struct.unpack("<f", struct.pack("<I", bits))[0])


def floor_bits(x: Fraction) -> int:
    """Largest non-negative finite binary32 bit pattern whose value is <= x (0 <= x < FLT_MAX)."""
    lo, hi = 0, 0x7F800000  # 0x7F800000 is +Inf; every pattern below it is finite
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if f32_value(mid) <= x:
            lo = mid
        else:
            hi = mid
    return lo


def check_margin(x: Fraction, boundary: Fraction, what: str) -> None:
    if abs(x - boundary) <= MARGIN * max(abs(x), Fraction(1, 10**30)):
        raise SystemExit(f"ambiguous rounding for {what}: value too close to {float(boundary)!r}")


def nearest_f32(x: Fraction, exact: bool, what: str) -> int:
    """Round to nearest binary32, ties to even."""
    lo = floor_bits(x)
    if f32_value(lo) == x:
        return lo
    mid = (f32_value(lo) + f32_value(lo + 1)) / 2
    if not exact:
        check_margin(x, mid, what)
    if x < mid:
        return lo
    if x > mid:
        return lo + 1
    return lo if lo % 2 == 0 else lo + 1


def ceil_f32(x: Fraction, exact: bool, what: str) -> int:
    """Smallest binary32 bit pattern whose value is >= x."""
    lo = floor_bits(x)
    if f32_value(lo) == x:
        if not exact:
            raise SystemExit(f"ambiguous ceiling for {what}")
        return lo
    if not exact:
        check_margin(x, f32_value(lo), what)
    return lo + 1


def srgb_to_linear(encoded: Fraction) -> tuple[Fraction, bool]:
    """EOTF; returns (value, exact)."""
    if encoded <= ENCODED_LIMIT:
        return encoded / Fraction(1292, 100), True
    base = (D(encoded.numerator) / D(encoded.denominator) + D("0.055")) / D("1.055")
    return Fraction(base ** D("2.4")), False


def linear_threshold_for_encoded(y: Fraction) -> tuple[Fraction, bool]:
    """Smallest linear x with OETF(x) >= y, where
    OETF(x) = 12.92 x for x <= 0.0031308, else 1.055 x^(1/2.4) - 0.055."""
    left_max = LINEAR_LIMIT * Fraction(1292, 100)
    right_min = D("1.055") * (D(LINEAR_LIMIT.numerator) / D(LINEAR_LIMIT.denominator)) ** (D(1) / D("2.4")) - D("0.055")
    if y <= left_max:
        return y / Fraction(1292, 100), True
    if y <= Fraction(right_min):
        raise SystemExit(f"threshold {float(y)!r} falls in the OETF segment gap")
    x = ((D(y.numerator) / D(y.denominator) + D("0.055")) / D("1.055")) ** D("2.4")
    if Fraction(x) <= LINEAR_LIMIT:
        raise SystemExit("power-segment inverse landed in the linear segment")
    return Fraction(x), False


def decimal(x: Fraction) -> Decimal:
    return D(x.numerator) / D(x.denominator)


def linear_to_srgb(linear: Fraction) -> tuple[Fraction, bool]:
    """OETF; returns (value, exact)."""
    if linear <= LINEAR_LIMIT:
        return linear * Fraction(1292, 100), True
    return Fraction(D("1.055") * decimal(linear) ** (D(1) / D("2.4")) - D("0.055")), False


# ---- sRGB -> XYZ (D50), decision N4: exact rational arithmetic ------------

def xy_to_xyz(x: Fraction, y: Fraction) -> list[Fraction]:
    return [x / y, Fraction(1), (1 - x - y) / y]


def inverse3(m):
    a, b, c = m[0]
    d, e, f = m[1]
    g, h, i = m[2]
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    return [[(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det],
            [(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det],
            [(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det]]


def matmul(a, b):
    return [[sum(a[r][k] * b[k][c] for k in range(3)) for c in range(3)] for r in range(3)]


def matvec(m, v):
    return [sum(m[r][k] * v[k] for k in range(3)) for r in range(3)]


def srgb_to_xyz_d50():
    """IEC 61966-2-1 primaries and D65, Bradford-adapted to the ICC D50 white (s15Fixed16)."""
    prims = [(Fraction(64, 100), Fraction(33, 100)), (Fraction(30, 100), Fraction(60, 100)),
             (Fraction(15, 100), Fraction(6, 100))]
    white = xy_to_xyz(Fraction(3127, 10000), Fraction(3290, 10000))
    d50 = [Fraction(63190, 65536), Fraction(1), Fraction(54061, 65536)]
    bradford = [[Fraction(s) for s in row] for row in
                (("0.8951", "0.2664", "-0.1614"), ("-0.7502", "1.7135", "0.0367"), ("0.0389", "-0.0685", "1.0296"))]
    columns = [xy_to_xyz(x, y) for x, y in prims]
    primaries = [[columns[c][r] for c in range(3)] for r in range(3)]
    scale = matvec(inverse3(primaries), white)
    to_xyz = [[primaries[r][c] * scale[c] for c in range(3)] for r in range(3)]
    source, target = matvec(bradford, white), matvec(bradford, d50)
    diagonal = [[target[i] / source[i] if i == j else Fraction(0) for j in range(3)] for i in range(3)]
    return matmul(matmul(inverse3(bradford), matmul(diagonal, bradford)), to_xyz)


class Lcg:
    """Deterministic 32-bit linear congruential generator (Numerical Recipes constants)."""

    def __init__(self, seed: int):
        self.state = seed

    def next(self) -> int:
        self.state = (self.state * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.state


def f32_bits(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


def write_pixel_format_oracle(path: str) -> None:
    matrix = srgb_to_xyz_d50()
    weights = matrix[1]

    # u16 decode: EOTF(v/65535) for a sample of v, densely around the segment junction.
    u16_values = sorted(set(range(0, 65536, 16)) | {65535} | set(range(2600, 2700)))
    u16_decode = []
    for v in u16_values:
        value, exact = srgb_to_linear(Fraction(v, 65535))
        u16_decode.append(nearest_f32(value, exact, f"EOTF({v}/65535)"))

    # u16 encode thresholds for a sample of levels k.
    u16_levels = sorted(set(range(0, 65535, 256)) | {65534} | set(range(2640, 2662)))
    u16_thresholds = []
    for k in u16_levels:
        x, exact = linear_threshold_for_encoded(Fraction(2 * k + 1, 131070))
        u16_thresholds.append(ceil_f32(x, exact, f"OETF^-1(({k}+0.5)/65535)"))

    # R'G'B' u8 -> Y' u8: Y = weights . EOTF(rgb/255), Y' = floor(OETF(Y)*255 + 0.5).
    rng = Lcg(0x5EED)
    rgb_cases = [(v, v, v) for v in range(256)] + [(255, 0, 0), (0, 255, 0), (0, 0, 255)]
    while len(rgb_cases) < 256 + 3 + 1024:
        r = rng.next()
        rgb_cases.append((r & 0xFF, (r >> 8) & 0xFF, (r >> 16) & 0xFF))
    gray_u8_inputs, gray_u8_expected, skipped_u8 = [], [], 0
    for rgb in rgb_cases:
        y = sum(weights[i] * srgb_to_linear(Fraction(rgb[i], 255))[0] for i in range(3))
        encoded, _ = linear_to_srgb(min(max(y, Fraction(0)), Fraction(1)))
        scaled = encoded * 255
        level = int(scaled + Fraction(1, 2))  # floor for non-negative values
        if abs(scaled - (level - Fraction(1, 2))) < Fraction(1, 10**9) or abs(scaled - (level + Fraction(1, 2))) < Fraction(1, 10**9):
            skipped_u8 += 1  # too close to a rounding tie for a double-precision implementation
            continue
        gray_u8_inputs.append(rgb[0] | rgb[1] << 8 | rgb[2] << 16)
        gray_u8_expected.append(level)

    # RGB float -> Y float: exact weighted sum of the binary32 inputs, rounded to nearest.
    float_cases = [(1.0, 1.0, 1.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0), (0.5, 0.25, 0.125)]
    while len(float_cases) < 1024:
        r, g, b = (struct.unpack("<f", struct.pack("<I", rng.next() >> 9 | 0x3F800000))[0] - 1.0 for _ in range(3))
        float_cases.append((r * 1.2 - 0.1, g * 1.2 - 0.1, b * 1.2 - 0.1))
    gray_f32_inputs, gray_f32_expected, skipped_f32 = [], [], 0
    for rgb in float_cases:
        floats = [struct.unpack("<f", struct.pack("<f", c))[0] for c in rgb]  # round to binary32
        y = sum(weights[i] * Fraction(floats[i]) for i in range(3))
        bits = nearest_f32(abs(y), True, "luminance") if y != 0 else 0
        if y != 0:
            # The library evaluates the sum in double with weights a few ulp
            # off; skip sums within 2^-40 (relative) of a binary32 midpoint.
            lo = floor_bits(abs(y))
            mid = (f32_value(lo) + f32_value(lo + 1)) / 2
            if abs(abs(y) - mid) < abs(y) * Fraction(1, 2**40):
                skipped_f32 += 1
                continue
        if y < 0:
            bits |= 0x80000000
        gray_f32_inputs.extend(f32_bits(c) for c in floats)
        gray_f32_expected.append(bits)

    def u32_table(name, comment, values):
        rows = ",\n".join("    " + ", ".join(f"0x{b:08X}u" for b in values[i : i + 6])
                          for i in range(0, len(values), 6))
        return f"/* {comment} */\nstatic const uint32_t {name}[{len(values)}] = {{\n{rows}\n}};\n\n"

    def u16_table(name, comment, values):
        rows = ",\n".join("    " + ", ".join(f"{v}" for v in values[i : i + 12]) for i in range(0, len(values), 12))
        return f"/* {comment} */\nstatic const uint16_t {name}[{len(values)}] = {{\n{rows}\n}};\n\n"

    def doubles(name, comment, values):
        body = ", ".join(float(v).hex() for v in values)
        return f"/* {comment} */\nstatic const double {name}[{len(values)}] = {{{body}}};\n\n"

    with open(path, "w", newline="\n") as f:
        f.write(
            "/* Generated by tools/oracle/generate_srgb_oracle.py; do not edit.\n"
            " * sRGB per decision N4 (IEC 61966-2-1 primaries, D65, Bradford, ICC D50\n"
            " * s15Fixed16), exact rational arithmetic; TRCs at 60 significant digits.\n"
            f" * Skipped as too close to call: {skipped_u8} gray u8 cases, {skipped_f32} gray float cases. */\n"
            "#ifndef CHROMAROUTE_PIXEL_FORMAT_ORACLE_DATA_H\n#define CHROMAROUTE_PIXEL_FORMAT_ORACLE_DATA_H\n\n"
            "#include <stdint.h>\n\n"
        )
        f.write(doubles("chromaroute_oracle_srgb_to_xyz_d50", "row-major, correctly rounded",
                        [matrix[r][c] for r in range(3) for c in range(3)]))
        f.write(u16_table("chromaroute_oracle_u16_decode_inputs", "u16 inputs", u16_values))
        f.write(u32_table("chromaroute_oracle_u16_decode_f32", "EOTF(v/65535) rounded to nearest", u16_decode))
        f.write(u16_table("chromaroute_oracle_u16_encode_levels", "levels k", u16_levels))
        f.write(u32_table("chromaroute_oracle_u16_encode_threshold",
                          "smallest float x with floor(OETF(x)*65535 + 0.5) >= k + 1", u16_thresholds))
        f.write(u32_table("chromaroute_oracle_gray_u8_inputs", "R | G << 8 | B << 16", gray_u8_inputs))
        f.write(u32_table("chromaroute_oracle_gray_u8_expected", "Y' u8", gray_u8_expected))
        f.write(u32_table("chromaroute_oracle_gray_f32_inputs", "linear R, G, B (binary32 bits)", gray_f32_inputs))
        f.write(u32_table("chromaroute_oracle_gray_f32_expected", "linear Y (binary32 bits)", gray_f32_expected))
        f.write("#endif\n")
    print(f"pixel-format oracle: skipped {skipped_u8} u8 and {skipped_f32} float gray cases")


def rgb_to_xyz_d50(chromaticities):
    """Exact RGB -> XYZ (D50) for (wx, wy, rx, ry, gx, gy, bx, by) given as decimal strings."""
    wx, wy, rx, ry, gx, gy, bx, by = (Fraction(v) for v in chromaticities)
    white = xy_to_xyz(wx, wy)
    d50 = [Fraction(63190, 65536), Fraction(1), Fraction(54061, 65536)]
    bradford = [[Fraction(s) for s in row] for row in
                (("0.8951", "0.2664", "-0.1614"), ("-0.7502", "1.7135", "0.0367"), ("0.0389", "-0.0685", "1.0296"))]
    columns = [xy_to_xyz(x, y) for x, y in ((rx, ry), (gx, gy), (bx, by))]
    primaries = [[columns[c][r] for c in range(3)] for r in range(3)]
    scale = matvec(inverse3(primaries), white)
    to_xyz = [[primaries[r][c] * scale[c] for c in range(3)] for r in range(3)]
    source, target = matvec(bradford, white), matvec(bradford, d50)
    diagonal = [[target[i] / source[i] if i == j else Fraction(0) for j in range(3)] for i in range(3)]
    return matmul(matmul(inverse3(bradford), matmul(diagonal, bradford)), to_xyz)


# Published chromaticities (decisions N4, N5), as the library defines its built-in spaces.
BUILTIN_SPACES = [
    ("sRGB", ("0.3127", "0.3290", "0.64", "0.33", "0.30", "0.60", "0.15", "0.06")),
    ("scRGB", ("0.3127", "0.3290", "0.64", "0.33", "0.30", "0.60", "0.15", "0.06")),
    ("Rec2020", ("0.3127", "0.3290", "0.708", "0.292", "0.170", "0.797", "0.131", "0.046")),
    ("DisplayP3", ("0.3127", "0.3290", "0.680", "0.320", "0.265", "0.690", "0.150", "0.060")),
    ("Adobish", ("0.3127", "0.3290", "0.64", "0.33", "0.21", "0.71", "0.15", "0.06")),
    ("ProPhoto", ("0.34567", "0.3585", "0.7347", "0.2653", "0.1596", "0.8404", "0.0366", "0.0001")),
    ("Apple", ("0.3127", "0.3290", "0.625", "0.34", "0.28", "0.595", "0.155", "0.07")),
    ("ACEScg", ("0.32168", "0.33767", "0.713", "0.293", "0.165", "0.830", "0.128", "0.044")),
    ("ACES2065-1", ("0.32168", "0.33767", "0.7347", "0.2653", "0.0", "1.0", "0.0001", "-0.0770")),
]


def gamma_pow(x: Fraction, exponent: Fraction) -> tuple[Fraction, bool]:
    """Pure power TRC: non-positive -> 0."""
    if x <= 0:
        return Fraction(0), True
    if x == 1:
        return Fraction(1), True
    return Fraction(decimal(x) ** decimal(exponent)), False


def nearest_f32_or_skip(value: Fraction, exact: bool, magnitude: Fraction):
    """Nearest binary32, or None when value lies within 2^-40 * magnitude of a midpoint
    (the library evaluates in double and cannot resolve closer than that)."""
    sign = -1 if value < 0 else 1
    a = abs(value)
    if a == 0:
        return 0
    lo = floor_bits(a)
    if f32_value(lo) != a:
        mid = (f32_value(lo) + f32_value(lo + 1)) / 2
        if abs(a - mid) < magnitude * Fraction(1, 2**40):
            return None
    bits = nearest_f32(a, True, "case")
    return bits | (0x80000000 if sign < 0 else 0)


def quantize_or_skip(encoded: Fraction, levels: int):
    """floor(clamp(e)*levels + 1/2), or None within 1e-9 LSB of a tie."""
    e = min(max(encoded, Fraction(0)), Fraction(1))
    scaled = e * levels
    level = int(scaled + Fraction(1, 2))
    for tie in (level - Fraction(1, 2), level + Fraction(1, 2)):
        if 0 < e < 1 and abs(scaled - tie) < Fraction(1, 10**9):
            return None
    return level


def write_color_semantics_oracle(path: str) -> None:
    matrices = {name: rgb_to_xyz_d50(c) for name, c in BUILTIN_SPACES}
    rng = Lcg(0xC0105)
    skipped = 0

    # Gamma 2.2 (Adobish) with the library's exponent: the binary64 value 2.2.
    g = Fraction(2.2)
    gamma_decode = []
    for v in range(256):
        value, exact = gamma_pow(Fraction(v, 255), g)
        gamma_decode.append(nearest_f32(value, exact, f"({v}/255)^2.2"))
    gamma_threshold = []
    for k in range(255):
        x, exact = gamma_pow(Fraction(2 * k + 1, 510), g)  # encoded (k+0.5)/255 -> linear threshold
        gamma_threshold.append(ceil_f32(x, exact, f"threshold {k}"))

    # Linear sRGB float -> linear Rec2020 float.
    to_rec = matmul(inverse3(matrices["Rec2020"]), matrices["sRGB"])
    cross_inputs, cross_expected = [], []
    while len(cross_expected) < 1024 * 3:
        rgb = [struct.unpack("<f", struct.pack("<I", rng.next() >> 9 | 0x3F800000))[0] * 1.2 - 1.3 for _ in range(3)]
        rgb = [struct.unpack("<f", struct.pack("<f", c))[0] for c in rgb]
        out = []
        for r in range(3):
            y = sum(to_rec[r][k] * Fraction(rgb[k]) for k in range(3))
            magnitude = sum(abs(to_rec[r][k] * Fraction(rgb[k])) for k in range(3))
            out.append(nearest_f32_or_skip(y, True, magnitude))
        if None in out:
            skipped += 1
            continue
        cross_inputs.extend(f32_bits(c) for c in rgb)
        cross_expected.extend(out)

    # R'G'B' u8 (sRGB) -> R'G'B' u8-Rec2020 (also sRGB TRC): decode, matrix, encode, quantize.
    u8_inputs, u8_expected = [], []
    while len(u8_expected) < 1024 * 3:
        r = rng.next()
        rgb = (r & 0xFF, (r >> 8) & 0xFF, (r >> 16) & 0xFF)
        linear = [srgb_to_linear(Fraction(c, 255))[0] for c in rgb]
        out = []
        for row in range(3):
            y = sum(to_rec[row][k] * linear[k] for k in range(3))
            out.append(quantize_or_skip(linear_to_srgb(y)[0], 255))
        if None in out:
            skipped += 1
            continue
        u8_inputs.append(rgb[0] | rgb[1] << 8 | rgb[2] << 16)
        u8_expected.extend(out)

    # R'G'B' u8-Rec2020 -> Y' u8-Rec2020: luminance with Rec2020's weights.
    weights = matrices["Rec2020"][1]
    gray_inputs, gray_expected = [], []
    while len(gray_expected) < 1024:
        r = rng.next()
        rgb = (r & 0xFF, (r >> 8) & 0xFF, (r >> 16) & 0xFF)
        y = sum(weights[k] * srgb_to_linear(Fraction(rgb[k], 255))[0] for k in range(3))
        level = quantize_or_skip(linear_to_srgb(y)[0], 255)
        if level is None:
            skipped += 1
            continue
        gray_inputs.append(rgb[0] | rgb[1] << 8 | rgb[2] << 16)
        gray_expected.append(level)

    # R'aG'aB'aA u8 -> RGBA float: color = EOTF((v/255) / used_alpha(a/255)).
    floor = Fraction(1, 65536)
    assoc_cases = [(v, a) for a in (0, 1, 2, 64, 128, 254, 255) for v in range(256)]
    while len(assoc_cases) < 7 * 256 + 1024:
        r = rng.next()
        assoc_cases.append((r & 0xFF, (r >> 8) & 0xFF))
    assoc_inputs, assoc_expected = [], []
    for v, a in assoc_cases:
        alpha = Fraction(a, 255)
        used = alpha if abs(alpha) > floor else floor
        value, exact = srgb_to_linear(Fraction(v, 255) / used)
        bits = nearest_f32_or_skip(value, exact, value)
        if bits is None:
            skipped += 1
            continue
        assoc_inputs.append(v | a << 8)
        assoc_expected.append(bits)

    def u32_table(name, comment, values):
        rows = ",\n".join("    " + ", ".join(f"0x{b:08X}u" for b in values[i : i + 6])
                          for i in range(0, len(values), 6))
        return f"/* {comment} */\nstatic const uint32_t {name}[{len(values)}] = {{\n{rows}\n}};\n\n"

    with open(path, "w", newline="\n") as f:
        f.write(
            "/* Generated by tools/oracle/generate_srgb_oracle.py; do not edit.\n"
            " * Built-in spaces (decisions N4, N5) in exact rational arithmetic; TRCs at\n"
            " * 60 significant digits; binary32 bit patterns.\n"
            f" * Cases skipped as too close to a rounding boundary to call: {skipped}. */\n"
            "#ifndef CHROMAROUTE_COLOR_SEMANTICS_ORACLE_DATA_H\n#define CHROMAROUTE_COLOR_SEMANTICS_ORACLE_DATA_H\n\n"
            "#include <stdint.h>\n\n"
        )
        f.write("/* Built-in spaces, RGB -> XYZ (D50), row-major, correctly rounded. */\n")
        f.write(f"static const char *const chromaroute_oracle_space_names[{len(BUILTIN_SPACES)}] = {{"
                + ", ".join(f'"{n}"' for n, _ in BUILTIN_SPACES) + "};\n")
        f.write(f"static const double chromaroute_oracle_space_rgb_to_xyz[{len(BUILTIN_SPACES)}][9] = {{\n")
        for name, _ in BUILTIN_SPACES:
            m = matrices[name]
            f.write("    {" + ", ".join(float(m[r][c]).hex() for r in range(3) for c in range(3)) + "},\n")
        f.write("};\n\n")
        f.write(u32_table("chromaroute_oracle_gamma22_u8_to_linear_f32", "(v/255)^2.2 rounded to nearest", gamma_decode))
        f.write(u32_table("chromaroute_oracle_gamma22_u8_threshold",
                          "[k]: smallest float x with floor(x^(1/2.2)*255 + 0.5) >= k + 1", gamma_threshold))
        f.write(u32_table("chromaroute_oracle_srgb_to_rec2020_f32_inputs", "linear sRGB R, G, B", cross_inputs))
        f.write(u32_table("chromaroute_oracle_srgb_to_rec2020_f32_expected", "linear Rec2020 R, G, B", cross_expected))
        f.write(u32_table("chromaroute_oracle_srgb_to_rec2020_u8_inputs", "R'G'B' u8: R | G << 8 | B << 16", u8_inputs))
        f.write(u32_table("chromaroute_oracle_srgb_to_rec2020_u8_expected", "R'G'B' u8-Rec2020 components", u8_expected))
        f.write(u32_table("chromaroute_oracle_rec2020_gray_u8_inputs", "R'G'B' u8-Rec2020: R | G << 8 | B << 16", gray_inputs))
        f.write(u32_table("chromaroute_oracle_rec2020_gray_u8_expected", "Y' u8-Rec2020", gray_expected))
        f.write(u32_table("chromaroute_oracle_associated_u8_inputs", "R'aG'aB'aA u8 color | alpha << 8", assoc_inputs))
        f.write(u32_table("chromaroute_oracle_associated_u8_expected", "linear color, binary32", assoc_expected))
        f.write("#endif\n")
    print(f"color-semantics oracle: skipped {skipped} cases")


def main() -> None:
    out_dir = sys.argv[1]
    write_color_semantics_oracle(f"{out_dir}/color_semantics_oracle_data.h")
    write_pixel_format_oracle(f"{out_dir}/pixel_format_oracle_data.h")
    out_path = f"{out_dir}/srgb_oracle_data.h"
    u8_to_f32 = [nearest_f32(Fraction(v, 255), True, f"{v}/255") for v in range(256)]
    srgb_u8_to_linear = []
    for v in range(256):
        value, exact = srgb_to_linear(Fraction(v, 255))
        srgb_u8_to_linear.append(nearest_f32(value, exact, f"EOTF({v}/255)"))
    f32_to_u8_threshold = [ceil_f32(Fraction(2 * k + 1, 510), True, f"({k}+0.5)/255") for k in range(255)]
    linear_to_srgb_threshold = []
    for k in range(255):
        x, exact = linear_threshold_for_encoded(Fraction(2 * k + 1, 510))
        linear_to_srgb_threshold.append(ceil_f32(x, exact, f"OETF^-1(({k}+0.5)/255)"))

    def table(name: str, comment: str, values: list[int]) -> str:
        rows = ",\n".join(
            "    " + ", ".join(f"0x{b:08X}u" for b in values[i : i + 6]) for i in range(0, len(values), 6)
        )
        return f"/* {comment} */\nstatic const uint32_t {name}[{len(values)}] = {{\n{rows}\n}};\n"

    with open(out_path, "w", newline="\n") as f:
        f.write(
            "/* Generated by tools/oracle/generate_srgb_oracle.py; do not edit.\n"
            " * IEC 61966-2-1 sRGB, evaluated at 60 significant digits and rounded\n"
            " * exactly to IEEE binary32. Values are binary32 bit patterns. */\n"
            "#ifndef CHROMAROUTE_SRGB_ORACLE_DATA_H\n#define CHROMAROUTE_SRGB_ORACLE_DATA_H\n\n"
            "#include <stdint.h>\n\n"
        )
        f.write(table("chromaroute_oracle_u8_to_f32", "v/255 rounded to nearest", u8_to_f32))
        f.write("\n")
        f.write(table("chromaroute_oracle_srgb_u8_to_linear_f32", "EOTF(v/255) rounded to nearest", srgb_u8_to_linear))
        f.write("\n")
        f.write(table("chromaroute_oracle_f32_to_u8_threshold",
                      "[k]: smallest float x with floor(x*255 + 0.5) >= k + 1", f32_to_u8_threshold))
        f.write("\n")
        f.write(table("chromaroute_oracle_linear_f32_to_srgb_u8_threshold",
                      "[k]: smallest float x with floor(OETF(x)*255 + 0.5) >= k + 1", linear_to_srgb_threshold))
        f.write("\n#endif\n")


if __name__ == "__main__":
    main()
