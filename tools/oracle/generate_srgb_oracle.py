"""Generates the high-precision reference data for the conversion tests.

Independent oracle (decision T4): IEC 61966-2-1 formulas evaluated with Python's
decimal module at 60 significant digits, then rounded exactly to IEEE binary32.
Branches that are rational are evaluated exactly with fractions.Fraction. Any
rounding decision closer than MARGIN to a boundary aborts generation instead of
guessing; sampled cases that are too close to call are skipped and counted.

Writes C++ headers <dir>/srgb_oracle_data.hpp (sRGB u8 <-> float, Milestone 1),
<dir>/pixel_format_oracle_data.hpp (u16, luminance and gray cases, Milestone 2),
<dir>/color_semantics_oracle_data.hpp (built-in spaces, gamma, alpha),
<dir>/cie_oracle_data.hpp (CIE XYZ, xyY, Yuv, Lab, LCH(ab), Milestone 4) and
<dir>/oklab_ycbcr_oracle_data.hpp (Oklab, Y'CbCr, Milestone 5).

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


# ---- C++ output -------------------------------------------------------------

def begin_header(f, comment: str, includes: list[str]) -> None:
    f.write("#pragma once\n\n")
    f.write("".join(f"// {line}".rstrip() + "\n" for line in comment.split("\n")))
    f.write("\n" + "".join(f"#include <{h}>\n" for h in includes))
    f.write("\nnamespace chromaroute::test::oracle {\n\n")


def end_header(f) -> None:
    f.write("} // namespace chromaroute::test::oracle\n")


def u32_table(name: str, comment: str, values: list[int]) -> str:
    rows = "\n".join("    " + ", ".join(f"0x{b:08X}u" for b in values[i : i + 6]) + ","
                     for i in range(0, len(values), 6))
    return f"// {comment}\ninline constexpr std::array<std::uint32_t, {len(values)}> {name} = {{\n{rows}\n}};\n\n"


def u16_table(name: str, comment: str, values: list[int]) -> str:
    rows = "\n".join("    " + ", ".join(f"{v}" for v in values[i : i + 12]) + ","
                     for i in range(0, len(values), 12))
    return f"// {comment}\ninline constexpr std::array<std::uint16_t, {len(values)}> {name} = {{\n{rows}\n}};\n\n"


def double_table(name: str, comment: str, values: list[Fraction]) -> str:
    body = ", ".join(float(v).hex() for v in values)
    return f"// {comment}\ninline constexpr std::array<double, {len(values)}> {name} = {{{body}}};\n\n"


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

    with open(path, "w", newline="\n") as f:
        begin_header(f,
                     "Generated by tools/oracle/generate_srgb_oracle.py; do not edit.\n"
                     "sRGB per decision N4 (IEC 61966-2-1 primaries, D65, Bradford, ICC D50\n"
                     "s15Fixed16), exact rational arithmetic; TRCs at 60 significant digits.\n"
                     f"Skipped as too close to call: {skipped_u8} gray u8 cases, {skipped_f32} gray float cases.",
                     ["array", "cstdint"])
        f.write(double_table("srgb_to_xyz_d50", "row-major, correctly rounded",
                             [matrix[r][c] for r in range(3) for c in range(3)]))
        f.write(u16_table("u16_decode_inputs", "u16 inputs", u16_values))
        f.write(u32_table("u16_decode_f32", "EOTF(v/65535) rounded to nearest", u16_decode))
        f.write(u16_table("u16_encode_levels", "levels k", u16_levels))
        f.write(u32_table("u16_encode_threshold",
                          "smallest float x with floor(OETF(x)*65535 + 0.5) >= k + 1", u16_thresholds))
        f.write(u32_table("gray_u8_inputs", "R | G << 8 | B << 16", gray_u8_inputs))
        f.write(u32_table("gray_u8_expected", "Y' u8", gray_u8_expected))
        f.write(u32_table("gray_f32_inputs", "linear R, G, B (binary32 bits)", gray_f32_inputs))
        f.write(u32_table("gray_f32_expected", "linear Y (binary32 bits)", gray_f32_expected))
        end_header(f)
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

    with open(path, "w", newline="\n") as f:
        begin_header(f,
                     "Generated by tools/oracle/generate_srgb_oracle.py; do not edit.\n"
                     "Built-in spaces (decisions N4, N5) in exact rational arithmetic; TRCs at\n"
                     "60 significant digits; binary32 bit patterns.\n"
                     f"Cases skipped as too close to a rounding boundary to call: {skipped}.",
                     ["array", "cstdint", "string_view"])
        f.write("// Built-in spaces, RGB -> XYZ (D50), row-major, correctly rounded.\n")
        f.write(f"inline constexpr std::array<std::string_view, {len(BUILTIN_SPACES)}> space_names = {{"
                + ", ".join(f'"{n}"' for n, _ in BUILTIN_SPACES) + "};\n")
        f.write(f"inline constexpr std::array<std::array<double, 9>, {len(BUILTIN_SPACES)}> space_rgb_to_xyz = {{{{\n")
        for name, _ in BUILTIN_SPACES:
            m = matrices[name]
            f.write("    {" + ", ".join(float(m[r][c]).hex() for r in range(3) for c in range(3)) + "},\n")
        f.write("}};\n\n")
        f.write(u32_table("gamma22_u8_to_linear_f32", "(v/255)^2.2 rounded to nearest", gamma_decode))
        f.write(u32_table("gamma22_u8_threshold",
                          "[k]: smallest float x with floor(x^(1/2.2)*255 + 0.5) >= k + 1", gamma_threshold))
        f.write(u32_table("srgb_to_rec2020_f32_inputs", "linear sRGB R, G, B", cross_inputs))
        f.write(u32_table("srgb_to_rec2020_f32_expected", "linear Rec2020 R, G, B", cross_expected))
        f.write(u32_table("srgb_to_rec2020_u8_inputs", "R'G'B' u8: R | G << 8 | B << 16", u8_inputs))
        f.write(u32_table("srgb_to_rec2020_u8_expected", "R'G'B' u8-Rec2020 components", u8_expected))
        f.write(u32_table("rec2020_gray_u8_inputs", "R'G'B' u8-Rec2020: R | G << 8 | B << 16", gray_inputs))
        f.write(u32_table("rec2020_gray_u8_expected", "Y' u8-Rec2020", gray_expected))
        f.write(u32_table("associated_u8_inputs", "R'aG'aB'aA u8 color | alpha << 8", assoc_inputs))
        f.write(u32_table("associated_u8_expected", "linear color, binary32", assoc_expected))
        end_header(f)
    print(f"color-semantics oracle: skipped {skipped} cases")


# ---- CIE models (Milestone 4): CIE 15:2004 at 60 digits -------------------

def d_cbrt(x: Decimal) -> Decimal:
    if x == 0:
        return D(0)
    a = abs(x)
    y = (a.ln() / 3).exp()
    y -= (y * y * y - a) / (3 * y * y)  # one Newton step on top of exp/ln
    return y if x > 0 else -y


def d_atan_small(x: Decimal) -> Decimal:
    """atan by halving the argument (atan x = 2 atan(x / (1 + sqrt(1 + x^2)))) and a Taylor series."""
    halvings = 0
    while abs(x) > D("0.001"):
        x = x / (1 + (1 + x * x).sqrt())
        halvings += 1
    total, power, n = D(0), x, 0
    while True:
        term = power / (2 * n + 1)
        if abs(term) < D(10) ** -75:
            break
        total += term if n % 2 == 0 else -term
        power *= x * x
        n += 1
    return total * (2 ** halvings)


PI = 16 * d_atan_small(D(1) / 5) - 4 * d_atan_small(D(1) / 239)  # Machin


def d_atan2(y: Decimal, x: Decimal) -> Decimal:
    if x > 0:
        return d_atan_small(y / x)
    if x < 0:
        return d_atan_small(y / x) + (PI if y >= 0 else -PI)
    return PI / 2 if y > 0 else (-PI / 2 if y < 0 else D(0))


def d_sin_cos(angle: Decimal) -> tuple[Decimal, Decimal]:
    two_pi = 2 * PI
    angle = angle - two_pi * (angle / two_pi).to_integral_value()
    s, c, term_s, term_c, n = D(0), D(0), angle, D(1), 0
    while abs(term_s) > D(10) ** -75 or abs(term_c) > D(10) ** -75:
        s += term_s
        c += term_c
        term_s = -term_s * angle * angle / ((2 * n + 2) * (2 * n + 3))
        term_c = -term_c * angle * angle / ((2 * n + 1) * (2 * n + 2))
        n += 1
    return s, c


D50 = [Fraction(63190, 65536), Fraction(1), Fraction(54061, 65536)]
LAB_EPSILON = Fraction(216, 24389)
LAB_KAPPA = Fraction(24389, 27)


def lab_f(t: Decimal) -> Decimal:
    return d_cbrt(t) if t > decimal(LAB_EPSILON) else (decimal(LAB_KAPPA) * t + 16) / 116


def lab_f_inverse(f: Decimal) -> Decimal:
    return f * f * f if f > D(6) / 29 else (116 * f - 16) / decimal(LAB_KAPPA)


def lab_from_xyz(xyz: list[Decimal]) -> list[Decimal]:
    fx, fy, fz = (lab_f(xyz[i] / decimal(D50[i])) for i in range(3))
    return [116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)]


def lab_magnitudes(xyz: list[Decimal]) -> list[Decimal]:
    """Size of the terms each L*a*b* component is computed from (for the skip rule)."""
    fx, fy, fz = (abs(lab_f(xyz[i] / decimal(D50[i]))) for i in range(3))
    return [116 * fy + 16, 500 * (fx + fy), 200 * (fy + fz)]


def xyz_from_lab(lab: list[Decimal]) -> list[Decimal]:
    fy = (lab[0] + 16) / 116
    ty = fy * fy * fy if lab[0] > 8 else lab[0] / decimal(LAB_KAPPA)
    t = [lab_f_inverse(fy + lab[1] / 500), ty, lab_f_inverse(fy - lab[2] / 200)]
    return [t[i] * decimal(D50[i]) for i in range(3)]


def lch_from_lab(lab: list[Decimal]) -> list[Decimal]:
    chroma = (lab[1] * lab[1] + lab[2] * lab[2]).sqrt()
    hue = d_atan2(lab[2], lab[1]) * 180 / PI
    if hue < 0:
        hue += 360
    return [lab[0], chroma, hue]


def f32_or_skip(value: Decimal, magnitude: Decimal):
    """Nearest binary32 bits, or None when the value lies within 2^-40 * magnitude of a
    binary32 midpoint or of zero (double evaluation cannot resolve closer)."""
    v = Fraction(value)
    window = Fraction(magnitude) * Fraction(1, 2**40)
    if abs(v) <= window:
        return None
    return nearest_f32_or_skip(v, True, Fraction(magnitude))


def u8_or_skip(value: Decimal, low: int, high: int):
    """floor((v - low) * 255 / (high - low) + 1/2) clamped to 0..255, or None near a tie."""
    scaled = (Fraction(value) - low) * 255 / (high - low)
    if scaled <= 0:
        return 0
    if scaled >= 255:
        return 255
    level = int(scaled + Fraction(1, 2))
    for tie in (level - Fraction(1, 2), level + Fraction(1, 2)):
        if abs(scaled - tie) < Fraction(1, 10**9):
            return None
    return level


def write_cie_oracle(path: str) -> None:
    matrix = rgb_to_xyz_d50(BUILTIN_SPACES[0][1])           # sRGB, exact rationals
    inverse = inverse3(matrix)
    prophoto = rgb_to_xyz_d50(BUILTIN_SPACES[5][1])
    rng = Lcg(0xC1E)
    skipped = 0

    def xyz_of(m, linear):
        return [sum(decimal(m[r][k]) * linear[k] for k in range(3)) for r in range(3)]

    def xyz_magnitudes(m, linear):
        return [sum(abs(decimal(m[r][k]) * linear[k]) for k in range(3)) for r in range(3)]

    # R'G'B' u8 (sRGB and ProPhoto) -> CIE Lab float and CIE Lab u8.
    def lab_cases(m, decode, count, extra):
        inputs, expected_f32, expected_u8 = [], [], []
        nonlocal skipped
        cases = list(extra)
        while len(cases) < count:
            r = rng.next()
            cases.append((r & 0xFF, (r >> 8) & 0xFF, (r >> 16) & 0xFF))
        for rgb in cases:
            linear = [decode(c) for c in rgb]
            xyz = xyz_of(m, linear)
            lab = lab_from_xyz(xyz)
            mags = lab_magnitudes(xyz)
            bits = [f32_or_skip(lab[i], mags[i]) for i in range(3)]
            codes = [u8_or_skip(lab[0], 0, 100), u8_or_skip(lab[1], -128, 127), u8_or_skip(lab[2], -128, 127)]
            if None in bits or None in codes:
                skipped += 1
                continue
            inputs.append(rgb[0] | rgb[1] << 8 | rgb[2] << 16)
            expected_f32.extend(bits)
            expected_u8.extend(codes)
        return inputs, expected_f32, expected_u8

    corners = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0), (0, 255, 255), (255, 0, 255)]
    srgb_in, srgb_lab, srgb_lab8 = lab_cases(matrix, lambda c: decimal(srgb_to_linear(Fraction(c, 255))[0]),
                                             1024, corners)
    gamma18 = Fraction(1.8)
    pro_in, pro_lab, pro_lab8 = lab_cases(prophoto, lambda c: decimal(gamma_pow(Fraction(c, 255), gamma18)[0]),
                                          256, corners)

    # sRGB gray u8 -> CIE L (a = b = 0 exactly for a gray source).
    gray_l = []
    for v in range(256):
        y = decimal(srgb_to_linear(Fraction(v, 255))[0])
        bits = f32_or_skip(116 * lab_f(y) - 16, 116 * abs(lab_f(y)) + 16) if v else 0
        if bits is None:
            raise SystemExit(f"gray L for {v} too close to call")
        gray_l.append(bits)

    # Linear sRGB float -> CIE XYZ, xyY, Yuv and LCH(ab) float.
    float_inputs, xyz_out, xyy_out, yuv_out, lch_out = [], [], [], [], []
    while len(float_inputs) < 512 * 3:
        rgb = [struct.unpack("<f", struct.pack("<I", rng.next() >> 9 | 0x3F800000))[0] * 1.2 - 1.3 for _ in range(3)]
        rgb = [struct.unpack("<f", struct.pack("<f", c))[0] for c in rgb]
        linear = [D(c) for c in rgb]
        xyz = xyz_of(matrix, linear)
        mags = xyz_magnitudes(matrix, linear)
        total = xyz[0] + xyz[1] + xyz[2]
        total_mag = mags[0] + mags[1] + mags[2]
        denominator = xyz[0] + 15 * xyz[1] + 3 * xyz[2]
        den_mag = mags[0] + 15 * mags[1] + 3 * mags[2]
        lab = lab_from_xyz(xyz)
        lch = lch_from_lab(lab)
        lab_mags = lab_magnitudes(xyz)
        if abs(total) < total_mag / 1000 or abs(denominator) < den_mag / 1000:
            skipped += 1  # chromaticities of near-black: badly conditioned
            continue
        candidates = [
            [f32_or_skip(xyz[i], mags[i]) for i in range(3)],
            [f32_or_skip(xyz[0] / total, total_mag / abs(total)), f32_or_skip(xyz[1] / total, total_mag / abs(total)),
             f32_or_skip(xyz[1], mags[1])],
            [f32_or_skip(xyz[1], mags[1]), f32_or_skip(4 * xyz[0] / denominator, 4 * den_mag / abs(denominator)),
             f32_or_skip(9 * xyz[1] / denominator, 9 * den_mag / abs(denominator))],
            [f32_or_skip(lch[0], lab_mags[0]), f32_or_skip(lch[1], lab_mags[1] + lab_mags[2]),
             f32_or_skip(lch[2], 360 * (lab_mags[1] + lab_mags[2]) / max(lch[1], D(10) ** -30))],
        ]
        if any(None in group for group in candidates):
            skipped += 1
            continue
        float_inputs.extend(f32_bits(c) for c in rgb)
        for out, group in zip((xyz_out, xyy_out, yuv_out, lch_out), candidates):
            out.extend(group)

    # CIE Lab float -> R'G'B' u8 (sRGB).
    lab_inputs, rgb_from_lab = [], []
    while len(rgb_from_lab) < 1024 * 3:
        values = [struct.unpack("<f", struct.pack("<I", rng.next() >> 9 | 0x3F800000))[0] - 1.0 for _ in range(3)]
        lab32 = [values[0] * 100.0, values[1] * 80.0 - 40.0, values[2] * 80.0 - 40.0]
        lab32 = [struct.unpack("<f", struct.pack("<f", c))[0] for c in lab32]
        xyz = xyz_from_lab([D(c) for c in lab32])
        linear = [sum(decimal(inverse[r][k]) * xyz[k] for k in range(3)) for r in range(3)]
        out = [quantize_or_skip(linear_to_srgb(Fraction(c))[0], 255) for c in linear]
        if None in out:
            skipped += 1
            continue
        lab_inputs.extend(f32_bits(c) for c in lab32)
        rgb_from_lab.extend(out)

    # The tables below draw after the ones above, which therefore stay unchanged.
    def unit() -> float:
        return struct.unpack("<f", struct.pack("<I", rng.next() >> 9 | 0x3F800000))[0] - 1.0

    def to_f32(x: float) -> float:
        return struct.unpack("<f", struct.pack("<f", x))[0]

    # Near black, linear sRGB float -> CIE Lab float. Every t <= epsilon, so L = kappa t,
    # a = 500 kappa / 116 (tx - ty) and b = 200 kappa / 116 (ty - tz) are exact rationals.
    # Magnitudes are those of the terms of that cancellation-free form.
    dark_inputs, dark_lab = [], []
    while len(dark_inputs) < 256 * 3:
        rgb = [to_f32((1 + 9 * unit()) * 10.0 ** -(4 + rng.next() % 27) * (-1 if rng.next() % 4 == 0 else 1))
               for _ in range(3)]
        exact = [Fraction(c) for c in rgb]
        t = [sum(matrix[r][k] * exact[k] for k in range(3)) / D50[r] for r in range(3)]
        t_mag = [sum(abs(matrix[r][k] * exact[k]) for k in range(3)) / D50[r] for r in range(3)]
        lab = [LAB_KAPPA * t[1], 500 * LAB_KAPPA / 116 * (t[0] - t[1]), 200 * LAB_KAPPA / 116 * (t[1] - t[2])]
        mags = [LAB_KAPPA * t_mag[1], 500 * LAB_KAPPA / 116 * (t_mag[0] + t_mag[1]),
                200 * LAB_KAPPA / 116 * (t_mag[1] + t_mag[2])]
        bits = [f32_or_skip(decimal(lab[i]), decimal(mags[i])) for i in range(3)]
        if None in bits:
            skipped += 1
            continue
        dark_inputs.extend(f32_bits(c) for c in rgb)
        dark_lab.extend(bits)

    # Near black, Y float -> CIE L float: L = kappa Y, down to binary32 subnormals.
    dark_y_inputs, dark_y_l = [], []
    for k in range(3, 46):
        y = to_f32(10.0 ** -k)
        bits = nearest_f32_or_skip(LAB_KAPPA * Fraction(y), True, 4 * LAB_KAPPA * Fraction(y))
        if bits is None:
            skipped += 1
            continue
        dark_y_inputs.append(f32_bits(y))
        dark_y_l.append(bits)

    # CIE LCH(ab) float -> CIE Lab float, hues in [-360, 720).
    lch_inputs, lch_lab = [], []
    while len(lch_inputs) < 512 * 3:
        lch32 = [to_f32(100 * unit()), to_f32(120 * unit()), to_f32(1080 * unit() - 360)]
        lch = [D(c) for c in lch32]
        angle = lch[2] * PI / 180
        sine, cosine = d_sin_cos(angle)
        size = lch[1] * (1 + abs(angle))
        bits = [f32_bits(lch32[0]), f32_or_skip(lch[1] * cosine, size), f32_or_skip(lch[1] * sine, size)]
        if None in bits:
            skipped += 1
            continue
        lch_inputs.extend(f32_bits(c) for c in lch32)
        lch_lab.extend(bits)

    # CIE xyY float and CIE Yuv float -> CIE XYZ float.
    xyy_inputs, xyy_xyz, yuv_inputs, yuv_xyz = [], [], [], []
    while len(xyy_inputs) < 256 * 3:
        x, y, big_y = to_f32(0.05 + 0.65 * unit()), to_f32(0.05 + 0.75 * unit()), to_f32(1.2 * unit())
        dx, dy, dY = D(x), D(y), D(big_y)
        bits = [f32_or_skip(dx * dY / dy, 4 * abs(dx * dY / dy)), f32_bits(big_y),
                f32_or_skip((1 - dx - dy) * dY / dy, 4 * (1 + dx + dy) * dY / dy)]
        if None in bits:
            skipped += 1
            continue
        xyy_inputs.extend(f32_bits(c) for c in (x, y, big_y))
        xyy_xyz.extend(bits)
    while len(yuv_inputs) < 256 * 3:
        big_y, u, v = to_f32(1.2 * unit()), to_f32(0.05 + 0.55 * unit()), to_f32(0.1 + 0.5 * unit())
        dY, du, dv = D(big_y), D(u), D(v)
        bits = [f32_or_skip(9 * du * dY / (4 * dv), 4 * abs(9 * du * dY / (4 * dv))), f32_bits(big_y),
                f32_or_skip(dY * (12 - 3 * du - 20 * dv) / (4 * dv), 4 * dY * (12 + 3 * du + 20 * dv) / (4 * dv))]
        if None in bits:
            skipped += 1
            continue
        yuv_inputs.extend(f32_bits(c) for c in (big_y, u, v))
        yuv_xyz.extend(bits)

    # CIE Lab float -> CIE XYZ float, R'G'B' u8-ProPhoto and Y' u8 (sRGB). One case in
    # eight is dark (L <= 10), for the linear branch of the inverse.
    prophoto_inverse = inverse3(prophoto)
    inverse_inputs, inverse_xyz, inverse_prophoto, inverse_gray = [], [], [], []
    while len(inverse_inputs) < 1024 * 3:
        lab32 = [to_f32((10 if rng.next() % 8 == 0 else 100) * unit()), to_f32(160 * unit() - 80),
                 to_f32(160 * unit() - 80)]
        lab = [D(c) for c in lab32]
        xyz = xyz_from_lab(lab)
        fy = (lab[0] + 16) / 116
        f_mag = [abs(fy) + abs(lab[1]) / 500, None, abs(fy) + abs(lab[2]) / 200]
        f = [fy + lab[1] / 500, None, fy - lab[2] / 200]
        t_mag = [0, 0, 0]
        for i in (0, 2):
            t_mag[i] = 3 * f[i] * f[i] * f_mag[i] if f[i] > D(6) / 29 else 116 * f_mag[i] / decimal(LAB_KAPPA)
        t_mag[1] = 3 * fy * fy * (abs(lab[0]) + 16) / 116 if lab[0] > 8 else abs(lab[0]) / decimal(LAB_KAPPA)
        xyz_bits = [f32_or_skip(xyz[i], t_mag[i] * decimal(D50[i])) for i in range(3)]
        linear = [sum(decimal(prophoto_inverse[r][k]) * xyz[k] for k in range(3)) for r in range(3)]
        pro = [quantize_or_skip(gamma_pow(Fraction(c), 1 / gamma18)[0], 255) for c in linear]
        gray = quantize_or_skip(linear_to_srgb(Fraction(xyz[1]))[0], 255)
        if None in xyz_bits or None in pro or gray is None:
            skipped += 1
            continue
        inverse_inputs.extend(f32_bits(c) for c in lab32)
        inverse_xyz.extend(xyz_bits)
        inverse_prophoto.extend(pro)
        inverse_gray.append(gray)

    with open(path, "w", newline="\n") as f:
        begin_header(f,
                     "Generated by tools/oracle/generate_srgb_oracle.py; do not edit.\n"
                     "CIE models per CIE 15:2004 (D50 ICC white, exact epsilon = 216/24389 and\n"
                     "kappa = 24389/27), spaces per decisions N4 and N5, evaluated at 60 digits;\n"
                     "binary32 bit patterns and u8 codes.\n"
                     f"Cases skipped as too close to call: {skipped}.",
                     ["array", "cstdint"])
        f.write(u32_table("srgb_u8_inputs", "R'G'B' u8 (sRGB): R | G << 8 | B << 16", srgb_in))
        f.write(u32_table("srgb_u8_to_lab_f32", "CIE Lab float L, a, b", srgb_lab))
        f.write(u32_table("srgb_u8_to_lab_u8", "CIE Lab u8 L, a, b codes", srgb_lab8))
        f.write(u32_table("prophoto_u8_inputs", "R'G'B' u8-ProPhoto: R | G << 8 | B << 16", pro_in))
        f.write(u32_table("prophoto_u8_to_lab_f32", "CIE Lab float L, a, b", pro_lab))
        f.write(u32_table("prophoto_u8_to_lab_u8", "CIE Lab u8 L, a, b codes", pro_lab8))
        f.write(u32_table("srgb_gray_u8_to_l_f32", "[v]: CIE L of R'G'B' u8 (v, v, v)", gray_l))
        f.write(u32_table("linear_f32_inputs", "linear sRGB R, G, B (binary32 bits)", float_inputs))
        f.write(u32_table("linear_f32_to_xyz_f32", "CIE XYZ float", xyz_out))
        f.write(u32_table("linear_f32_to_xyy_f32", "CIE xyY float", xyy_out))
        f.write(u32_table("linear_f32_to_yuv_f32", "CIE Yuv float (Y, u', v')", yuv_out))
        f.write(u32_table("linear_f32_to_lch_f32", "CIE LCH(ab) float (L, C, H degrees)", lch_out))
        f.write(u32_table("lab_f32_inputs", "CIE Lab float L, a, b (binary32 bits)", lab_inputs))
        f.write(u32_table("lab_f32_to_srgb_u8", "R'G'B' u8 (sRGB) components", rgb_from_lab))
        f.write(u32_table("dark_f32_inputs", "linear sRGB R, G, B near black (binary32 bits)", dark_inputs))
        f.write(u32_table("dark_f32_to_lab_f32", "CIE Lab float L, a, b", dark_lab))
        f.write(u32_table("dark_y_f32_inputs", "Y float near black (binary32 bits)", dark_y_inputs))
        f.write(u32_table("dark_y_f32_to_l_f32", "CIE L float", dark_y_l))
        f.write(u32_table("lch_f32_inputs", "CIE LCH(ab) float L, C, H degrees (binary32 bits)", lch_inputs))
        f.write(u32_table("lch_f32_to_lab_f32", "CIE Lab float L, a, b", lch_lab))
        f.write(u32_table("xyy_f32_inputs", "CIE xyY float x, y, Y (binary32 bits)", xyy_inputs))
        f.write(u32_table("xyy_f32_to_xyz_f32", "CIE XYZ float", xyy_xyz))
        f.write(u32_table("yuv_f32_inputs", "CIE Yuv float Y, u', v' (binary32 bits)", yuv_inputs))
        f.write(u32_table("yuv_f32_to_xyz_f32", "CIE XYZ float", yuv_xyz))
        f.write(u32_table("lab_inverse_f32_inputs", "CIE Lab float L, a, b (binary32 bits)", inverse_inputs))
        f.write(u32_table("lab_inverse_f32_to_xyz_f32", "CIE XYZ float", inverse_xyz))
        f.write(u32_table("lab_inverse_f32_to_prophoto_u8", "R'G'B' u8-ProPhoto components", inverse_prophoto))
        f.write(u32_table("lab_inverse_f32_to_gray_u8", "Y' u8 (sRGB)", inverse_gray))
        end_header(f)
    print(f"CIE oracle: skipped {skipped} cases")


# ---- Oklab and Y'CbCr (Milestone 5) --------------------------------------

def bradford_adaptation(source_white, target_white):
    """Exact Bradford adaptation matrix between two XYZ whites (as in rgb_to_xyz_d50)."""
    bradford = [[Fraction(v) for v in row] for row in
                (("0.8951", "0.2664", "-0.1614"), ("-0.7502", "1.7135", "0.0367"), ("0.0389", "-0.0685", "1.0296"))]
    source, target = matvec(bradford, source_white), matvec(bradford, target_white)
    diagonal = [[target[i] / source[i] if i == j else Fraction(0) for j in range(3)] for i in range(3)]
    return matmul(inverse3(bradford), matmul(diagonal, bradford))


# CSS Color 4 sample code (decision S3): XYZ (D65) -> LMS, and LMS' -> Oklab.
OKLAB_M1 = [[Fraction(v) for v in row] for row in (
    ("0.8190224379967030", "0.3619062600528904", "-0.1288737815209879"),
    ("0.0329836539323885", "0.9292868615863434", "0.0361446663506424"),
    ("0.0481771893596242", "0.2642395317527308", "0.6335478284694309"))]
OKLAB_M2 = [[Fraction(v) for v in row] for row in (
    ("0.2104542683093140", "0.7936177747023054", "-0.0040720430116193"),
    ("1.9779985324311684", "-2.4285922420485799", "0.4505937096174110"),
    ("0.0259040424655478", "0.7827717124575296", "-0.8086757549230774"))]
D65_SRGB = xy_to_xyz(Fraction("0.3127"), Fraction("0.3290"))


def write_oklab_ycbcr_oracle(path: str) -> None:
    srgb = rgb_to_xyz_d50(BUILTIN_SPACES[0][1])
    srgb_inverse = inverse3(srgb)
    prophoto = rgb_to_xyz_d50(BUILTIN_SPACES[5][1])
    m1 = matmul(OKLAB_M1, bradford_adaptation(D50, D65_SRGB))  # D50 XYZ -> LMS
    m1_inverse, m2_inverse = inverse3(m1), inverse3(OKLAB_M2)
    rng = Lcg(0x0C1AB)
    skipped = 0

    def unit() -> float:
        return struct.unpack("<f", struct.pack("<I", rng.next() >> 9 | 0x3F800000))[0] - 1.0

    def to_f32(x: float) -> float:
        return struct.unpack("<f", struct.pack("<f", x))[0]

    def srgb_decode(code: int) -> Decimal:
        return decimal(srgb_to_linear(Fraction(code, 255))[0])

    def prophoto_decode(code: int) -> Decimal:
        return decimal(gamma_pow(Fraction(code, 255), Fraction(1.8))[0])

    def dmatvec(m, v):
        return [sum(decimal(m[r][k]) * v[k] for k in range(3)) for r in range(3)]

    def dmags(m, v):
        return [sum(abs(decimal(m[r][k]) * v[k]) for k in range(3)) for r in range(3)]

    def oklab_of_xyz(xyz):
        lms = dmatvec(m1, xyz)
        root = [d_cbrt(x) for x in lms]
        return dmatvec(OKLAB_M2, root), dmags(OKLAB_M2, root)

    def oklab_bits(xyz):
        lab, mags = oklab_of_xyz(xyz)
        return [f32_or_skip(lab[i], mags[i]) for i in range(3)]

    def random_codes(count, extra):
        cases = list(extra)
        while len(cases) < count:
            r = rng.next()
            cases.append((r & 0xFF, (r >> 8) & 0xFF, (r >> 16) & 0xFF))
        return cases

    corners = [(255, 255, 255), (0, 0, 0), (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0), (0, 255, 255),
               (255, 0, 255), (128, 128, 128)]

    # R'G'B' u8 (sRGB, ProPhoto) -> Oklab float.
    def oklab_cases(m, decode, count):
        nonlocal skipped
        inputs, out = [], []
        for rgb in random_codes(count, corners):
            bits = oklab_bits(dmatvec(m, [decode(c) for c in rgb]))
            if None in bits:
                skipped += 1
                continue
            inputs.append(rgb[0] | rgb[1] << 8 | rgb[2] << 16)
            out.extend(bits)
        return inputs, out

    srgb_in, srgb_oklab = oklab_cases(srgb, srgb_decode, 1024)
    pro_in, pro_oklab = oklab_cases(prophoto, prophoto_decode, 256)

    # Linear sRGB float -> Oklab float, including out-of-gamut values.
    linear_in, linear_oklab = [], []
    while len(linear_in) < 512 * 3:
        rgb = [to_f32(1.5 * unit() - 0.25) for _ in range(3)]
        bits = oklab_bits(dmatvec(srgb, [D(c) for c in rgb]))
        if None in bits:
            skipped += 1
            continue
        linear_in.extend(f32_bits(c) for c in rgb)
        linear_oklab.extend(bits)

    # Oklab float -> CIE XYZ float and R'G'B' u8 (sRGB).
    inverse_in, inverse_xyz, inverse_rgb = [], [], []
    while len(inverse_in) < 1024 * 3:
        lab32 = [to_f32(unit()), to_f32(0.6 * unit() - 0.3), to_f32(0.6 * unit() - 0.3)]
        lab = [D(c) for c in lab32]
        root = dmatvec(m2_inverse, lab)
        root_mags = dmags(m2_inverse, lab)
        lms = [x * x * x for x in root]
        lms_mags = [3 * root_mags[i] * root[i] * root[i] for i in range(3)]
        xyz = dmatvec(m1_inverse, lms)
        xyz_mags = [sum(abs(decimal(m1_inverse[r][k])) * lms_mags[k] for k in range(3)) for r in range(3)]
        xyz_bits = [f32_or_skip(xyz[i], xyz_mags[i]) for i in range(3)]
        linear = dmatvec(srgb_inverse, xyz)
        codes = [quantize_or_skip(linear_to_srgb(Fraction(c))[0], 255) for c in linear]
        if None in xyz_bits or None in codes:
            skipped += 1
            continue
        inverse_in.extend(f32_bits(c) for c in lab32)
        inverse_xyz.extend(xyz_bits)
        inverse_rgb.extend(codes)

    # Y'CbCr from R~G~B~ with exact weights: R' = code / 255 in sRGB (the
    # R'G'B' TRC is the sRGB curve there).
    def ycbcr_exact(rgb, kr, kb):
        kg = 1 - kr - kb
        y = kr * rgb[0] + kg * rgb[1] + kb * rgb[2]
        return [y, (rgb[2] - y) / (2 * (1 - kb)), (rgb[0] - y) / (2 * (1 - kr))]

    def ycbcr_mags(rgb, kr, kb):
        y = abs(rgb[1]) + abs(kr * (rgb[0] - rgb[1])) + abs(kb * (rgb[2] - rgb[1]))
        return [y, (abs(rgb[2]) + y) / (2 * (1 - kb)), (abs(rgb[0]) + y) / (2 * (1 - kr))]

    bt601 = (Fraction("0.299"), Fraction("0.114"))
    bt709 = (Fraction("0.2126"), Fraction("0.0722"))

    def code_or_skip(value: Fraction, lo: Fraction, hi: Fraction, c_lo: int, c_hi: int):
        """Round half up of c_lo + (v - lo)(c_hi - c_lo)/(hi - lo), clamped; None at an exact tie."""
        if value <= lo:
            return c_lo
        if value >= hi:
            return c_hi
        scaled = c_lo + (value - lo) * (c_hi - c_lo) / (hi - lo)
        if scaled - int(scaled) == Fraction(1, 2):
            return None
        return int(scaled + Fraction(1, 2))

    ycc_in, ycc601, ycc709, ycc601_u8 = [], [], [], []
    for rgb in random_codes(1024, corners):
        exact = [Fraction(c, 255) for c in rgb]
        f601 = ycbcr_exact(exact, *bt601)
        m601 = ycbcr_mags(exact, *bt601)
        f709 = ycbcr_exact(exact, *bt709)
        m709 = ycbcr_mags(exact, *bt709)
        bits = [nearest_f32_or_skip(f601[i], True, m601[i]) for i in range(3)] + \
               [nearest_f32_or_skip(f709[i], True, m709[i]) for i in range(3)]
        codes = [code_or_skip(f601[0], Fraction(0), Fraction(1), 16, 235)] + \
                [code_or_skip(f601[i], Fraction(-1, 2), Fraction(1, 2), 16, 240) for i in (1, 2)]
        if None in bits or None in codes:
            skipped += 1
            continue
        ycc_in.append(rgb[0] | rgb[1] << 8 | rgb[2] << 16)
        ycc601.extend(bits[:3])
        ycc709.extend(bits[3:])
        ycc601_u8.extend(codes)

    # R'G'B' u8-ProPhoto -> Y'CbCr float-ProPhoto: decode the gamma 1.8, then the
    # sRGB curve on ProPhoto's linear RGB (S4).
    pro_ycc_in, pro_ycc = [], []
    for rgb in random_codes(256, corners):
        tilde = [Fraction(linear_to_srgb(Fraction(prophoto_decode(c)))[0]) for c in rgb]
        values = ycbcr_exact(tilde, *bt601)
        mags = ycbcr_mags(tilde, *bt601)
        bits = [nearest_f32_or_skip(values[i], True, mags[i]) for i in range(3)]
        if None in bits:
            skipped += 1
            continue
        pro_ycc_in.append(rgb[0] | rgb[1] << 8 | rgb[2] << 16)
        pro_ycc.extend(bits)

    # Y'CbCr u8 codes -> R'G'B' u8 (sRGB), and Y'CbCr709 float -> R'G'B' u8.
    def rgb_from_ycbcr(ycc, kr, kb):
        kg = 1 - kr - kb
        r = ycc[0] + 2 * (1 - kr) * ycc[2]
        b = ycc[0] + 2 * (1 - kb) * ycc[1]
        return [r, (ycc[0] - kr * r - kb * b) / kg, b]

    u8_in, u8_rgb = [], []
    while len(u8_in) < 1024:
        codes = [16 + rng.next() % 220, 16 + rng.next() % 225, 16 + rng.next() % 225]
        ycc = [Fraction(codes[0] - 16, 219), Fraction(codes[1] - 128, 224), Fraction(codes[2] - 128, 224)]
        out = [quantize_or_skip(c, 255) for c in rgb_from_ycbcr(ycc, *bt601)]
        if None in out:
            skipped += 1
            continue
        u8_in.append(codes[0] | codes[1] << 8 | codes[2] << 16)
        u8_rgb.extend(out)
    f709_in, f709_rgb = [], []
    while len(f709_in) < 512 * 3:
        ycc32 = [to_f32(unit()), to_f32(unit() - 0.5), to_f32(unit() - 0.5)]
        out = [quantize_or_skip(c, 255) for c in rgb_from_ycbcr([Fraction(c) for c in ycc32], *bt709)]
        if None in out:
            skipped += 1
            continue
        f709_in.extend(f32_bits(c) for c in ycc32)
        f709_rgb.extend(out)

    with open(path, "w", newline="\n") as f:
        begin_header(f,
                     "Generated by tools/oracle/generate_srgb_oracle.py; do not edit.\n"
                     "Oklab with the CSS Color 4 matrices after Bradford adaptation from the\n"
                     "ICC D50 white to D65 (x 0.3127, y 0.3290), decision S3; Y'CbCr from the\n"
                     "sRGB-curve encoding with BT.601 and BT.709 weights, decision S4. Exact\n"
                     "rationals, cube roots at 60 digits; binary32 bit patterns and u8 codes.\n"
                     f"Cases skipped as too close to call: {skipped}.",
                     ["array", "cstdint"])
        f.write(u32_table("oklab_srgb_u8_inputs", "R'G'B' u8 (sRGB): R | G << 8 | B << 16", srgb_in))
        f.write(u32_table("srgb_u8_to_oklab_f32", "Oklab float L, a, b", srgb_oklab))
        f.write(u32_table("oklab_prophoto_u8_inputs", "R'G'B' u8-ProPhoto: R | G << 8 | B << 16", pro_in))
        f.write(u32_table("prophoto_u8_to_oklab_f32", "Oklab float L, a, b", pro_oklab))
        f.write(u32_table("oklab_linear_f32_inputs", "linear sRGB R, G, B (binary32 bits)", linear_in))
        f.write(u32_table("linear_f32_to_oklab_f32", "Oklab float L, a, b", linear_oklab))
        f.write(u32_table("oklab_f32_inputs", "Oklab float L, a, b (binary32 bits)", inverse_in))
        f.write(u32_table("oklab_f32_to_xyz_f32", "CIE XYZ float (D50)", inverse_xyz))
        f.write(u32_table("oklab_f32_to_srgb_u8", "R'G'B' u8 (sRGB) components", inverse_rgb))
        f.write(u32_table("ycbcr_srgb_u8_inputs", "R'G'B' u8 (sRGB): R | G << 8 | B << 16", ycc_in))
        f.write(u32_table("srgb_u8_to_ycbcr_f32", "Y'CbCr float (BT.601)", ycc601))
        f.write(u32_table("srgb_u8_to_ycbcr709_f32", "Y'CbCr709 float", ycc709))
        f.write(u32_table("srgb_u8_to_ycbcr_u8", "Y'CbCr u8 codes", ycc601_u8))
        f.write(u32_table("ycbcr_prophoto_u8_inputs", "R'G'B' u8-ProPhoto: R | G << 8 | B << 16", pro_ycc_in))
        f.write(u32_table("prophoto_u8_to_ycbcr_f32", "Y'CbCr float-ProPhoto", pro_ycc))
        f.write(u32_table("ycbcr_u8_inputs", "Y'CbCr u8: Y' | Cb << 8 | Cr << 16", u8_in))
        f.write(u32_table("ycbcr_u8_to_srgb_u8", "R'G'B' u8 (sRGB) components", u8_rgb))
        f.write(u32_table("ycbcr709_f32_inputs", "Y'CbCr709 float (binary32 bits)", f709_in))
        f.write(u32_table("ycbcr709_f32_to_srgb_u8", "R'G'B' u8 (sRGB) components", f709_rgb))
        end_header(f)
    print(f"Oklab and Y'CbCr oracle: skipped {skipped} cases")


# ---- HSL, HSV, HCY (decision S4): exact rationals on R~G~B~ ------------------

HUE_F32_ONE = 0x3F800000


def hue_of(rgb, high, chroma):
    """Hexagonal hue in [0, 1) of a color with chroma > 0."""
    r, g, b = rgb
    if high == r:
        sextant = (g - b) / chroma
        if sextant < 0:
            sextant += 6
    elif high == g:
        sextant = 2 + (b - r) / chroma
    else:
        sextant = 4 + (r - g) / chroma
    return sextant / 6


def hue_color(hue):
    """The most chromatic color of a hue in [0, 1): largest channel 1, smallest 0."""
    sextant = 6 * hue
    sector = min(int(sextant), 5)
    f = sextant - sector
    return [(1, f, 0), (1 - f, 1, 0), (0, 1, f), (0, 1 - f, 1), (f, 0, 1), (1, 0, 1 - f)][sector]


def hcy_luma(rgb, weights):
    return sum(weights[i] * rgb[i] for i in range(3))


def hue_models_exact(rgb, weights):
    """HSL, HSV and HCY of an R~G~B~ color with values and magnitudes for the skip
    windows: (value, magnitude) per component, or None where a model has no
    saturation."""
    high, low = max(rgb), min(rgb)
    chroma = high - low
    size = abs(high) + abs(low)
    y = hcy_luma(rgb, weights)
    y_mag = sum(abs(weights[i] * rgb[i]) for i in range(3)) + size
    lightness = (high + low) / 2
    if chroma == 0:
        return ([(0, 1), (0, 1), (lightness, size)], [(0, 1), (0, 1), (high, size)], [(0, 1), (0, 1), (y, y_mag)])
    hue = hue_of(rgb, high, chroma)
    ratio = 1 + size / chroma  # relative error growth from max - min and the differences
    hue_mag = 1 + ratio
    scale = high + low if high + low <= 1 else 2 - high - low
    hsl = [(hue, hue_mag), (chroma / scale, abs(chroma / scale) * (ratio + (size + 2) / abs(scale))) if scale != 0 else None,
           (lightness, size)]
    hsv = [(hue, hue_mag), (chroma / high, abs(chroma / high) * (ratio + size / abs(high))) if high != 0 else None,
           (high, size)]
    peak = hcy_luma([(c - low) / chroma for c in rgb], weights)
    below = y < peak
    divisor = y if below else 1 - y
    reference = peak if below else 1 - peak
    if divisor != 0:
        rel = chroma * reference / divisor
        rel_mag = abs(rel) * (ratio + 2 + (y_mag + 1) / abs(divisor) + 2 / abs(reference))
        hcy = [(hue, hue_mag), (rel, rel_mag), (y, y_mag)]
    else:
        hcy = [(hue, hue_mag), None, (y, y_mag)]
    return hsl, hsv, hcy


def hue_f32_or_skip(components):
    """Binary32 bits of (hue, saturation, third) with the stored-hue rules: a hue
    that rounds to 1 is stored as 0, and so is the hue of a saturation that
    rounds to 0. None when a component is too close to call or undefined."""
    if None in components:
        return None
    bits = []
    for value, magnitude in components:
        value = Fraction(value)
        magnitude = Fraction(magnitude)
        window = magnitude * Fraction(1, 2**40)
        if value != 0 and abs(value) <= window:
            return None
        b = nearest_f32_or_skip(value, True, magnitude)
        if b is None:
            return None
        bits.append(b)
    if bits[0] == HUE_F32_ONE or (bits[1] & 0x7FFFFFFF) == 0:
        bits[0] = 0
    return bits


def rgb_from_hue_model(model, values, weights):
    """R~G~B~ of HSL, HSV or HCY values (hue in [0, 1)) and a magnitude per channel."""
    hue, sat, third = values
    if sat == 0:
        return [third] * 3, abs(third) + 1
    color = hue_color(hue)
    if model == "hsl":
        chroma = (2 * third if 2 * third <= 1 else 2 - 2 * third) * sat
        low = third - chroma / 2
    elif model == "hsv":
        chroma = sat * third
        low = third - chroma
    else:
        peak = hcy_luma(color, weights)
        below = third < peak
        chroma = sat * (third if below else 1 - third) / (peak if below else 1 - peak)
        low = third - chroma * peak
    return [low + chroma * c for c in color], abs(third) + 4 * abs(chroma) + 1


def write_hue_oracle(path: str) -> None:
    srgb_y = rgb_to_xyz_d50(BUILTIN_SPACES[0][1])[1]
    prophoto = rgb_to_xyz_d50(BUILTIN_SPACES[5][1])
    prophoto_y = prophoto[1]
    assert sum(srgb_y) == 1 and sum(prophoto_y) == 1
    rng = Lcg(0x45C1)
    skipped = 0

    def unit() -> float:
        return struct.unpack("<f", struct.pack("<I", rng.next() >> 9 | 0x3F800000))[0] - 1.0

    def to_f32(x: float) -> float:
        return struct.unpack("<f", struct.pack("<f", x))[0]

    def random_codes(count, extra):
        cases = list(extra)
        while len(cases) < count:
            r = rng.next()
            cases.append((r & 0xFF, (r >> 8) & 0xFF, (r >> 16) & 0xFF))
        return cases

    corners = [(255, 255, 255), (0, 0, 0), (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0), (0, 255, 255),
               (255, 0, 255), (128, 128, 128), (255, 0, 1), (1, 0, 0), (254, 255, 255), (10, 10, 11)]

    def forward(tilde_of, codes, weights):
        nonlocal skipped
        inputs, out = [], [[], [], []]
        for rgb in codes:
            models = [hue_f32_or_skip(m) for m in hue_models_exact(tilde_of(rgb), weights)]
            if None in models:
                skipped += 1
                continue
            inputs.append(rgb[0] | rgb[1] << 8 | rgb[2] << 16)
            for i in range(3):
                out[i].extend(models[i])
        return inputs, out

    # R'G'B' u8 (sRGB) -> HSL, HSV, HCY float: R~ = code / 255 exactly there.
    srgb_in, srgb_out = forward(lambda rgb: [Fraction(c, 255) for c in rgb], random_codes(1024, corners), srgb_y)

    # R'G'B' u8-ProPhoto -> float-ProPhoto: decode the gamma 1.8, then the sRGB
    # curve on ProPhoto's linear RGB (S4); HCY with ProPhoto's luma weights.
    def prophoto_tilde(rgb):
        return [linear_to_srgb(gamma_pow(Fraction(c, 255), Fraction(18, 10))[0])[0] for c in rgb]

    pro_in, pro_out = forward(prophoto_tilde, random_codes(256, corners), prophoto_y)

    # Linear sRGB float -> HSV and HCY float, including values outside [0, 1].
    linear_in, linear_hsv, linear_hcy = [], [], []
    while len(linear_in) < 512 * 3:
        rgb = [to_f32(1.5 * unit() - 0.25) for _ in range(3)]
        tilde = [linear_to_srgb(Fraction(c))[0] for c in rgb]
        _, hsv, hcy = hue_models_exact(tilde, srgb_y)
        hsv_bits, hcy_bits = hue_f32_or_skip(hsv), hue_f32_or_skip(hcy)
        if hsv_bits is None or hcy_bits is None:
            skipped += 1
            continue
        linear_in.extend(f32_bits(c) for c in rgb)
        linear_hsv.extend(hsv_bits)
        linear_hcy.extend(hcy_bits)

    # HSL, HSV, HCY float -> R'G'B' u8 and R'G'B' float (sRGB).
    def inverse(model):
        nonlocal skipped
        inputs, codes_out, floats_out = [], [], []
        while len(inputs) < 512 * 3:
            values32 = [to_f32(unit()), to_f32(unit()), to_f32(unit())]
            if len(inputs) < 8 * 3:
                values32[1] = 0.0 if len(inputs) < 4 * 3 else 1.0
            rgb, magnitude = rgb_from_hue_model(model, [Fraction(v) for v in values32], srgb_y)
            codes = [quantize_or_skip(c, 255) for c in rgb]
            floats = [nearest_f32_or_skip(c, True, magnitude) for c in rgb]
            if None in codes or None in floats or any(c != 0 and abs(c) <= magnitude * Fraction(1, 2**40) for c in rgb):
                skipped += 1
                continue
            inputs.extend(f32_bits(v) for v in values32)
            codes_out.extend(codes)
            floats_out.extend(floats)
        return inputs, codes_out, floats_out

    inverse_tables = {model: inverse(model) for model in ("hsl", "hsv", "hcy")}

    with open(path, "w", newline="\n") as f:
        begin_header(f,
                     "Generated by tools/oracle/generate_srgb_oracle.py; do not edit.\n"
                     "HSL, HSV and HCY from the sRGB-curve encoding of the space (decision S4),\n"
                     "hue in turns; HCY with the space's luma weights as exact rationals. Exact\n"
                     "rationals (ProPhoto's curve at 60 digits); binary32 bit patterns and u8\n"
                     "codes, with the stored-hue rules applied.\n"
                     f"Cases skipped as too close to call: {skipped}.",
                     ["array", "cstdint"])
        f.write(u32_table("hue_srgb_u8_inputs", "R'G'B' u8 (sRGB): R | G << 8 | B << 16", srgb_in))
        f.write(u32_table("hue_srgb_u8_to_hsl_f32", "HSL float", srgb_out[0]))
        f.write(u32_table("hue_srgb_u8_to_hsv_f32", "HSV float", srgb_out[1]))
        f.write(u32_table("hue_srgb_u8_to_hcy_f32", "HCY float", srgb_out[2]))
        f.write(u32_table("hue_prophoto_u8_inputs", "R'G'B' u8-ProPhoto: R | G << 8 | B << 16", pro_in))
        f.write(u32_table("hue_prophoto_u8_to_hsl_f32", "HSL float-ProPhoto", pro_out[0]))
        f.write(u32_table("hue_prophoto_u8_to_hsv_f32", "HSV float-ProPhoto", pro_out[1]))
        f.write(u32_table("hue_prophoto_u8_to_hcy_f32", "HCY float-ProPhoto", pro_out[2]))
        f.write(u32_table("hue_linear_f32_inputs", "linear sRGB R, G, B (binary32 bits)", linear_in))
        f.write(u32_table("hue_linear_f32_to_hsv_f32", "HSV float", linear_hsv))
        f.write(u32_table("hue_linear_f32_to_hcy_f32", "HCY float", linear_hcy))
        for model in ("hsl", "hsv", "hcy"):
            inputs, codes, floats = inverse_tables[model]
            f.write(u32_table(f"hue_{model}_f32_inputs", f"{model.upper()} float (binary32 bits)", inputs))
            f.write(u32_table(f"hue_{model}_f32_to_srgb_u8", "R'G'B' u8 (sRGB) components", codes))
            f.write(u32_table(f"hue_{model}_f32_to_srgb_f32", "R'G'B' float (sRGB, binary32 bits)", floats))
        end_header(f)
    print(f"HSL, HSV and HCY oracle: skipped {skipped} cases")


def main() -> None:
    out_dir = sys.argv[1]
    write_hue_oracle(f"{out_dir}/hue_oracle_data.hpp")
    write_oklab_ycbcr_oracle(f"{out_dir}/oklab_ycbcr_oracle_data.hpp")
    write_cie_oracle(f"{out_dir}/cie_oracle_data.hpp")
    write_color_semantics_oracle(f"{out_dir}/color_semantics_oracle_data.hpp")
    write_pixel_format_oracle(f"{out_dir}/pixel_format_oracle_data.hpp")
    out_path = f"{out_dir}/srgb_oracle_data.hpp"
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

    with open(out_path, "w", newline="\n") as f:
        begin_header(f,
                     "Generated by tools/oracle/generate_srgb_oracle.py; do not edit.\n"
                     "IEC 61966-2-1 sRGB, evaluated at 60 significant digits and rounded\n"
                     "exactly to IEEE binary32. Values are binary32 bit patterns.",
                     ["array", "cstdint"])
        f.write(u32_table("u8_to_f32", "v/255 rounded to nearest", u8_to_f32))
        f.write(u32_table("srgb_u8_to_linear_f32", "EOTF(v/255) rounded to nearest", srgb_u8_to_linear))
        f.write(u32_table("f32_to_u8_threshold",
                          "[k]: smallest float x with floor(x*255 + 0.5) >= k + 1", f32_to_u8_threshold))
        f.write(u32_table("linear_f32_to_srgb_u8_threshold",
                          "[k]: smallest float x with floor(OETF(x)*255 + 0.5) >= k + 1", linear_to_srgb_threshold))
        end_header(f)


if __name__ == "__main__":
    main()
