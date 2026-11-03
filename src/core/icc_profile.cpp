#include "core/icc_profile.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace chromaroute::detail {
namespace {

constexpr std::size_t header_size = 128;
constexpr std::size_t tag_entry_size = 12;
constexpr std::uint32_t max_curve_entries = 65536;

Error malformed(const std::string& message)
{
    return Error{ErrorCode::invalid_profile, "invalid ICC profile: " + message};
}

Error unsupported(const std::string& message)
{
    return Error{ErrorCode::unsupported_profile, "unsupported ICC profile: " + message};
}

constexpr std::uint32_t signature(const char (&s)[5])
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(s[0])) << 24 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(s[1])) << 16 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(s[2])) << 8 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(s[3]));
}

// The seven profile classes of ICC.1 (versions 2 and 4).
bool definedClass(std::uint32_t sig) noexcept
{
    for (std::uint32_t c : {signature("scnr"), signature("mntr"), signature("prtr"), signature("link"),
                            signature("spac"), signature("abst"), signature("nmcl")}) {
        if (sig == c) {
            return true;
        }
    }
    return false;
}

// The data color spaces of ICC.1: the named ones, and 2 to 15 colorants
// ('2CLR' to '9CLR', 'ACLR' to 'FCLR').
bool definedColorSpace(std::uint32_t sig) noexcept
{
    for (std::uint32_t s : {signature("XYZ "), signature("Lab "), signature("Luv "), signature("YCbr"),
                            signature("Yxy "), signature("RGB "), signature("GRAY"), signature("HSV "),
                            signature("HLS "), signature("CMYK"), signature("CMY ")}) {
        if (sig == s) {
            return true;
        }
    }
    const auto count = static_cast<char>(sig >> 24);
    return (sig & 0xFFFFFF) == (signature("xCLR") & 0xFFFFFF) &&
           ((count >= '2' && count <= '9') || (count >= 'A' && count <= 'F'));
}

// A signature as text for messages, with non-printing bytes as '?'.
std::string text(std::uint32_t sig)
{
    std::string s(4, ' ');
    for (int i = 0; i < 4; ++i) {
        const auto c = static_cast<char>((sig >> (24 - 8 * i)) & 0xFF);
        s[static_cast<std::size_t>(i)] = c >= 0x20 && c < 0x7F ? c : '?';
    }
    return "'" + s + "'";
}

// Big-endian reads. Callers check ranges with has() first.
class Bytes {
public:
    explicit Bytes(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] bool has(std::uint64_t offset, std::uint64_t length) const noexcept
    {
        return offset <= bytes_.size() && length <= bytes_.size() - offset;
    }
    [[nodiscard]] std::uint8_t u8(std::size_t at) const noexcept { return std::to_integer<std::uint8_t>(bytes_[at]); }
    [[nodiscard]] std::uint16_t u16(std::size_t at) const noexcept
    {
        return static_cast<std::uint16_t>(u8(at) << 8 | u8(at + 1));
    }
    [[nodiscard]] std::uint32_t u32(std::size_t at) const noexcept
    {
        return static_cast<std::uint32_t>(u8(at)) << 24 | static_cast<std::uint32_t>(u8(at + 1)) << 16 |
               static_cast<std::uint32_t>(u8(at + 2)) << 8 | static_cast<std::uint32_t>(u8(at + 3));
    }
    // s15Fixed16Number, exact in double.
    [[nodiscard]] double s15f16(std::size_t at) const noexcept
    {
        return static_cast<std::int32_t>(u32(at)) / 65536.0;
    }

private:
    std::span<const std::byte> bytes_;
};

struct Tag {
    std::uint32_t signature;
    std::uint32_t offset;
    std::uint32_t size;
};

class Profile {
public:
    Profile(Bytes bytes, std::vector<Tag> tags) : bytes_(bytes), tags_(std::move(tags)) {}

    // The first tag with the signature.
    [[nodiscard]] const Tag* find(std::uint32_t sig) const noexcept
    {
        for (const Tag& t : tags_) {
            if (t.signature == sig) {
                return &t;
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool hasLookupTables() const noexcept
    {
        for (std::uint32_t sig : {signature("A2B0"), signature("A2B1"), signature("A2B2"), signature("B2A0"),
                                  signature("B2A1"), signature("B2A2")}) {
            if (find(sig) != nullptr) {
                return true;
            }
        }
        return false;
    }

    Result<Vector3> xyz(std::uint32_t sig) const
    {
        const Tag* t = find(sig);
        if (t == nullptr) {
            return missing(sig);
        }
        if (t->size < 20 || bytes_.u32(t->offset) != signature("XYZ ")) {
            return malformed("tag " + text(sig) + " is not an XYZType of one value");
        }
        return Vector3{bytes_.s15f16(t->offset + 8), bytes_.s15f16(t->offset + 12), bytes_.s15f16(t->offset + 16)};
    }

    Result<IccCurve> curve(std::uint32_t sig) const
    {
        const Tag* t = find(sig);
        if (t == nullptr) {
            return missing(sig);
        }
        if (t->size < 12) {
            return malformed("tag " + text(sig) + " is too short for a curve");
        }
        const std::uint32_t type = bytes_.u32(t->offset);
        IccCurve c;
        if (type == signature("curv")) {
            const std::uint32_t count = bytes_.u32(t->offset + 8);
            if (count > (t->size - 12) / 2) {
                return malformed("curve " + text(sig) + " has more entries than its tag holds");
            }
            if (count > max_curve_entries) {
                return malformed("curve " + text(sig) + " has more than 65536 entries");
            }
            if (count == 0) {
                c.kind = IccCurve::Kind::identity;
            } else if (count == 1) {
                c.kind = IccCurve::Kind::gamma;
                c.gamma = bytes_.u16(t->offset + 12) / 256.0;
            } else {
                c.kind = IccCurve::Kind::sampled;
                c.samples.resize(count);
                for (std::uint32_t i = 0; i < count; ++i) {
                    c.samples[i] = bytes_.u16(t->offset + 12 + 2 * i) / 65535.0;
                }
            }
            return c;
        }
        if (type == signature("para")) {
            static constexpr std::uint32_t parameters[5] = {1, 3, 4, 5, 7};
            const std::uint16_t function = bytes_.u16(t->offset + 8);
            if (function > 4) {
                return malformed("parametric curve " + text(sig) + " has unknown type " + std::to_string(function));
            }
            if (t->size < 12 + 4 * parameters[function]) {
                return malformed("parametric curve " + text(sig) + " is shorter than its parameters");
            }
            double p[7] = {};
            for (std::uint32_t i = 0; i < parameters[function]; ++i) {
                p[i] = bytes_.s15f16(t->offset + 12 + 4 * i);
            }
            c.kind = IccCurve::Kind::parametric;
            c.parametric = ParametricCurve{function, p[0], p[1], p[2], p[3], p[4], p[5], p[6]};
            return c;
        }
        return malformed("tag " + text(sig) + " is neither a curveType nor a parametricCurveType");
    }

private:
    // A missing matrix/TRC tag: a profile built on lookup tables alone is
    // valid but unsupported; otherwise the profile is incomplete.
    Error missing(std::uint32_t sig) const
    {
        if (hasLookupTables()) {
            return unsupported("only lookup tables, no " + text(sig) + " tag (matrix/TRC profiles are supported)");
        }
        return malformed("missing tag " + text(sig));
    }

    Bytes bytes_;
    std::vector<Tag> tags_;
};

} // namespace

Result<IccMatrixProfile> readIccMatrixProfile(std::span<const std::byte> profile)
{
    if (profile.size() < header_size + 4) {
        return malformed("shorter than a header and tag count (132 bytes)");
    }
    const Bytes all(profile);
    const std::uint32_t declared = all.u32(0);
    if (declared < header_size + 4 || declared > profile.size()) {
        return malformed("declared size " + std::to_string(declared) + " does not fit the " +
                         std::to_string(profile.size()) + " bytes given");
    }
    const Bytes bytes(profile.first(declared));
    if (bytes.u32(36) != signature("acsp")) {
        return malformed("no 'acsp' signature");
    }
    const std::uint8_t major = bytes.u8(8);
    if (major != 2 && major != 4) {
        return unsupported("version " + std::to_string(major) + " (versions 2 and 4 are supported)");
    }
    const std::uint32_t device_class = bytes.u32(12);
    if (!definedClass(device_class)) {
        return malformed("unknown profile class " + text(device_class));
    }
    if (device_class == signature("link") || device_class == signature("abst") || device_class == signature("nmcl")) {
        return unsupported("device class " + text(device_class));
    }
    const std::uint32_t color_space = bytes.u32(16);
    if (!definedColorSpace(color_space)) {
        return malformed("unknown color space " + text(color_space));
    }
    const bool gray = color_space == signature("GRAY");
    if (!gray && color_space != signature("RGB ")) {
        return unsupported("color space " + text(color_space) + " (RGB and GRAY are supported)");
    }
    // ICC.1 defines three-component matrix/TRC profiles for the input and
    // display classes, and monochrome ones for the output class too; the other
    // classes are built on lookup tables.
    const bool matrix_class = device_class == signature("scnr") || device_class == signature("mntr");
    if (!matrix_class && !(gray && device_class == signature("prtr"))) {
        return unsupported((gray ? "GRAY" : "RGB") + std::string(" profile of class ") + text(device_class) +
                           " (matrix/TRC data is defined for input and display profiles, and gray also for output)");
    }
    const std::uint32_t pcs = bytes.u32(20);
    if (pcs == signature("Lab ")) {
        return unsupported("Lab connection space (matrix/TRC profiles use XYZ)");
    }
    if (pcs != signature("XYZ ")) {
        return malformed("connection space " + text(pcs));
    }

    const std::uint32_t count = bytes.u32(header_size);
    if (count > (declared - header_size - 4) / tag_entry_size) {
        return malformed("tag table larger than the profile");
    }
    std::vector<Tag> tags;
    tags.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t entry = header_size + 4 + tag_entry_size * i;
        const Tag tag{bytes.u32(entry), bytes.u32(entry + 4), bytes.u32(entry + 8)};
        if (!bytes.has(tag.offset, tag.size)) {
            return malformed("tag " + text(tag.signature) + " lies outside the profile");
        }
        tags.push_back(tag);
    }
    const Profile p(bytes, std::move(tags));

    IccMatrixProfile result;
    result.gray = gray;
    if (gray) {
        auto k = p.curve(signature("kTRC"));
        if (!k) {
            return k.error();
        }
        result.trc[0] = std::move(*k);
        return result;
    }
    const std::uint32_t colorants[3] = {signature("rXYZ"), signature("gXYZ"), signature("bXYZ")};
    const std::uint32_t curves[3] = {signature("rTRC"), signature("gTRC"), signature("bTRC")};
    for (std::size_t c = 0; c < 3; ++c) {
        const auto column = p.xyz(colorants[c]);
        if (!column) {
            return column.error();
        }
        for (std::size_t r = 0; r < 3; ++r) {
            result.rgb_to_xyz[r][c] = (*column)[r];
        }
        auto curve = p.curve(curves[c]);
        if (!curve) {
            return curve.error();
        }
        result.trc[c] = std::move(*curve);
    }
    const auto white = p.xyz(signature("wtpt"));
    if (!white) {
        return white.error();
    }
    result.white = *white;
    return result;
}

} // namespace chromaroute::detail
