#pragma once

// Builds ICC profiles for the tests: a 128-byte header, a tag table and tag
// data, all big-endian, each tag padded to 4 bytes (ICC.1 sections 7 and 10).

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace chromaroute::test {

// The s15Fixed16Number nearest to v.
inline std::int32_t s15(double v)
{
    return static_cast<std::int32_t>(std::lround(v * 65536.0));
}

class IccWriter {
public:
    explicit IccWriter(std::uint8_t major = 4, const char* color_space = "RGB ", const char* pcs = "XYZ ",
                       const char* device_class = "mntr")
        : major_(major), color_space_(color_space), pcs_(pcs), device_class_(device_class)
    {
    }

    IccWriter& xyz(const char* sig, std::int32_t x, std::int32_t y, std::int32_t z)
    {
        std::vector<std::byte> d = typed("XYZ ");
        put32(d, static_cast<std::uint32_t>(x));
        put32(d, static_cast<std::uint32_t>(y));
        put32(d, static_cast<std::uint32_t>(z));
        return raw(sig, d);
    }

    IccWriter& xyz(const char* sig, double x, double y, double z) { return xyz(sig, s15(x), s15(y), s15(z)); }

    // curveType: no entries is the identity, one a u8Fixed8 gamma, more a table.
    IccWriter& curve(const char* sig, const std::vector<std::uint16_t>& entries)
    {
        std::vector<std::byte> d = typed("curv");
        put32(d, static_cast<std::uint32_t>(entries.size()));
        for (std::uint16_t e : entries) {
            put16(d, e);
        }
        return raw(sig, d);
    }

    IccWriter& para(const char* sig, std::uint16_t type, const std::vector<std::int32_t>& parameters)
    {
        std::vector<std::byte> d = typed("para");
        put16(d, type);
        put16(d, 0);
        for (std::int32_t p : parameters) {
            put32(d, static_cast<std::uint32_t>(p));
        }
        return raw(sig, d);
    }

    // lut16Type ('mft2', ICC.1 10.10): `inputs` channels to `outputs`, an
    // identity matrix and identity input and output tables (2 entries), and a
    // CLUT of `grid` points per input. `node` gets a node's inputs in [0, 1]
    // (the first input varying slowest) and returns its `outputs` values in
    // [0, 1], stored as uInt16.
    template <typename Node>
    IccWriter& lut16(const char* sig, std::size_t inputs, std::size_t outputs, std::size_t grid, Node node)
    {
        std::vector<std::byte> d = typed("mft2");
        d.push_back(std::byte{static_cast<unsigned char>(inputs)});
        d.push_back(std::byte{static_cast<unsigned char>(outputs)});
        d.push_back(std::byte{static_cast<unsigned char>(grid)});
        d.push_back(std::byte{0});
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                put32(d, static_cast<std::uint32_t>(r == c ? 65536 : 0));
            }
        }
        put16(d, 2);
        put16(d, 2);
        for (std::size_t i = 0; i < inputs; ++i) {
            put16(d, 0);
            put16(d, 65535);
        }
        std::size_t nodes = 1;
        for (std::size_t i = 0; i < inputs; ++i) {
            nodes *= grid;
        }
        std::vector<double> at(inputs);
        for (std::size_t n = 0; n < nodes; ++n) {
            std::size_t rest = n;
            for (std::size_t i = inputs; i-- > 0;) {
                at[i] = static_cast<double>(rest % grid) / static_cast<double>(grid - 1);
                rest /= grid;
            }
            const std::vector<double> values = node(at);
            for (std::size_t o = 0; o < outputs; ++o) {
                const double v = values[o] < 0.0 ? 0.0 : values[o] > 1.0 ? 1.0 : values[o];
                put16(d, static_cast<std::uint16_t>(std::lround(v * 65535.0)));
            }
        }
        for (std::size_t o = 0; o < outputs; ++o) {
            put16(d, 0);
            put16(d, 65535);
        }
        return raw(sig, d);
    }

    IccWriter& raw(const char* sig, const std::vector<std::byte>& data)
    {
        tags_.push_back({sig, data});
        return *this;
    }

    // An RGB matrix/TRC profile with sRGB's D50 colorants and one curve tag
    // written by `trc` for all three channels.
    template <typename Curve>
    static IccWriter rgb(Curve trc, std::uint8_t major = 4)
    {
        IccWriter w(major);
        w.xyz("rXYZ", 0.4360747, 0.2225045, 0.0139322)
            .xyz("gXYZ", 0.3850649, 0.7168786, 0.0971045)
            .xyz("bXYZ", 0.1430804, 0.0606169, 0.7141733)
            .xyz("wtpt", 0.9642, 1.0, 0.8249);
        for (const char* sig : {"rTRC", "gTRC", "bTRC"}) {
            trc(w, sig);
        }
        return w;
    }

    [[nodiscard]] std::vector<std::byte> bytes() const
    {
        std::vector<std::byte> out(128, std::byte{0});
        out[8] = std::byte{major_};
        out[9] = std::byte{static_cast<unsigned char>(major_ == 4 ? 0x40 : 0x20)};
        putSig(out, 12, device_class_);
        putSig(out, 16, color_space_);
        putSig(out, 20, pcs_);
        putSig(out, 36, "acsp");
        put32(out, static_cast<std::uint32_t>(tags_.size()));
        const std::size_t table = out.size();
        out.resize(table + 12 * tags_.size());
        for (std::size_t i = 0; i < tags_.size(); ++i) {
            while (out.size() % 4 != 0) {
                out.push_back(std::byte{0});
            }
            const std::size_t offset = out.size();
            out.insert(out.end(), tags_[i].data.begin(), tags_[i].data.end());
            putSig(out, table + 12 * i, tags_[i].signature.c_str());
            set32(out, table + 12 * i + 4, static_cast<std::uint32_t>(offset));
            set32(out, table + 12 * i + 8, static_cast<std::uint32_t>(tags_[i].data.size()));
        }
        set32(out, 0, static_cast<std::uint32_t>(out.size()));
        return out;
    }

    static void set32(std::vector<std::byte>& d, std::size_t at, std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i) {
            d[at + static_cast<std::size_t>(i)] = std::byte{static_cast<unsigned char>(v >> (24 - 8 * i))};
        }
    }

private:
    struct Tag {
        std::string signature;
        std::vector<std::byte> data;
    };

    static std::vector<std::byte> typed(const char* type)
    {
        std::vector<std::byte> d(8, std::byte{0});
        putSig(d, 0, type);
        return d;
    }
    static void putSig(std::vector<std::byte>& d, std::size_t at, const char* sig)
    {
        for (std::size_t i = 0; i < 4; ++i) {
            d[at + i] = std::byte{static_cast<unsigned char>(sig[i])};
        }
    }
    static void put16(std::vector<std::byte>& d, std::uint16_t v)
    {
        d.push_back(std::byte{static_cast<unsigned char>(v >> 8)});
        d.push_back(std::byte{static_cast<unsigned char>(v)});
    }
    static void put32(std::vector<std::byte>& d, std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i) {
            d.push_back(std::byte{static_cast<unsigned char>(v >> (24 - 8 * i))});
        }
    }

    std::uint8_t major_;
    const char* color_space_;
    const char* pcs_;
    const char* device_class_;
    std::vector<Tag> tags_;
};

} // namespace chromaroute::test
