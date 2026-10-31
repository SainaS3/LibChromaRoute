#include "engine/conversion_plan.hpp"

#include "engine/cie.hpp"
#include "engine/color_math.hpp"
#include "engine/half.hpp"
#include "engine/numeric.hpp"
#include "engine/oklab.hpp"
#include "engine/transfer.hpp"
#include "engine/ycbcr.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <utility>

namespace chromaroute::detail {
namespace {

// Pixels per pipeline pass; the double scratch buffers live on the stack.
constexpr std::size_t chunk_pixels = 128;

template <typename Storage>
Storage load(const std::byte* in) noexcept
{
    Storage value;
    std::memcpy(&value, in, sizeof value); // buffers may be unaligned
    return value;
}

template <typename Storage>
void store(std::byte* out, Storage value) noexcept
{
    std::memcpy(out, &value, sizeof value);
}

template <typename UInt>
void unpackUnorm(const TypeDescriptor&, const std::byte* in, std::size_t byte_stride, double* out,
                 std::size_t value_stride, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i) {
        out[i * value_stride] = decodeUnorm(load<UInt>(in + i * byte_stride));
    }
}

template <typename UInt>
void packUnorm(const TypeDescriptor&, const double* in, std::size_t value_stride, std::byte* out,
               std::size_t byte_stride, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i) {
        store(out + i * byte_stride, encodeUnorm<UInt>(in[i * value_stride]));
    }
}

// The highest code of a range type: code_max, or the storage's largest code.
template <typename UInt>
double highestCode(const TypeDescriptor& type) noexcept
{
    return type.code_max != 0 ? static_cast<double>(type.code_max) : static_cast<double>(std::numeric_limits<UInt>::max());
}

template <typename UInt>
void unpackRange(const TypeDescriptor& type, const std::byte* in, std::size_t byte_stride, double* out,
                 std::size_t value_stride, std::size_t count)
{
    const double code_lo = type.code_min;
    const double code_hi = highestCode<UInt>(type);
    for (std::size_t i = 0; i < count; ++i) {
        out[i * value_stride] = decodeRange(load<UInt>(in + i * byte_stride), type.min, type.max, code_lo, code_hi);
    }
}

template <typename UInt>
void packRange(const TypeDescriptor& type, const double* in, std::size_t value_stride, std::byte* out,
               std::size_t byte_stride, std::size_t count)
{
    const double code_lo = type.code_min;
    const double code_hi = highestCode<UInt>(type);
    for (std::size_t i = 0; i < count; ++i) {
        store(out + i * byte_stride, encodeRange<UInt>(in[i * value_stride], type.min, type.max, code_lo, code_hi));
    }
}

void unpackHalf(const TypeDescriptor&, const std::byte* in, std::size_t byte_stride, double* out,
                std::size_t value_stride, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i) {
        out[i * value_stride] = decodeHalf(load<std::uint16_t>(in + i * byte_stride));
    }
}

void packHalf(const TypeDescriptor&, const double* in, std::size_t value_stride, std::byte* out,
              std::size_t byte_stride, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i) {
        store(out + i * byte_stride, encodeHalf(in[i * value_stride]));
    }
}

void unpackF32(const TypeDescriptor&, const std::byte* in, std::size_t byte_stride, double* out,
               std::size_t value_stride, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i) {
        out[i * value_stride] = load<float>(in + i * byte_stride);
    }
}

void packF32(const TypeDescriptor&, const double* in, std::size_t value_stride, std::byte* out,
             std::size_t byte_stride, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i) {
        store(out + i * byte_stride, narrowToFloat(in[i * value_stride]));
    }
}

void unpackF64(const TypeDescriptor&, const std::byte* in, std::size_t byte_stride, double* out,
               std::size_t value_stride, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i) {
        out[i * value_stride] = load<double>(in + i * byte_stride);
    }
}

void packF64(const TypeDescriptor&, const double* in, std::size_t value_stride, std::byte* out,
             std::size_t byte_stride, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i) {
        store(out + i * byte_stride, in[i * value_stride]);
    }
}

ConversionPlan::UnpackFunction unpackFor(const TypeDescriptor& type)
{
    switch (type.type) {
        case ComponentType::u8: return type.ranged() ? unpackRange<std::uint8_t> : unpackUnorm<std::uint8_t>;
        case ComponentType::u16: return type.ranged() ? unpackRange<std::uint16_t> : unpackUnorm<std::uint16_t>;
        case ComponentType::u32: return type.ranged() ? nullptr : unpackUnorm<std::uint32_t>;
        case ComponentType::f16: return unpackHalf;
        case ComponentType::f32: return unpackF32;
        case ComponentType::f64: return unpackF64;
    }
    return nullptr;
}

ConversionPlan::PackFunction packFor(const TypeDescriptor& type)
{
    switch (type.type) {
        case ComponentType::u8: return type.ranged() ? packRange<std::uint8_t> : packUnorm<std::uint8_t>;
        case ComponentType::u16: return type.ranged() ? packRange<std::uint16_t> : packUnorm<std::uint16_t>;
        case ComponentType::u32: return type.ranged() ? nullptr : packUnorm<std::uint32_t>;
        case ComponentType::f16: return packHalf;
        case ComponentType::f32: return packF32;
        case ComponentType::f64: return packF64;
    }
    return nullptr;
}

// The TRC a model applies to channel c in a space; nullptr for linear and CIE
// models. Gray models use the space's first (red) TRC, perceptual models always sRGB.
const TrcDescriptor* trcOf(const ModelDescriptor& model, const SpaceDescriptor& space, std::size_t channel)
{
    switch (model.encoding) {
        case Encoding::linear: return nullptr;
        case Encoding::space_trc: return space.trc[model.family == ColorFamily::gray ? 0 : channel];
        case Encoding::perceptual: return &srgbTrc();
        case Encoding::none: return nullptr;
    }
    return nullptr;
}

using Hub = ConversionPlan::Hub;

// A CIE or Oklab model's colors -> the hub.
Vector3 cieToHub(ColorFamily family, Hub hub, const Vector3& v) noexcept
{
    if (hub == Hub::same) {
        return v;
    }
    if (hub == Hub::lab) {
        return family == ColorFamily::cie_lch ? labFromLch(v) : v;
    }
    const auto xyz = [&] {
        switch (family) {
            case ColorFamily::cie_xyy: return xyzFromXyy(v);
            case ColorFamily::cie_yuv: return xyzFromYuv(v);
            case ColorFamily::oklab: return xyzFromOklab(v);
            default: return v;
        }
    };
    if (hub == Hub::xyz) {
        return xyz();
    }
    switch (family) {
        case ColorFamily::cie_lab: return normalizedFromLab(v);
        case ColorFamily::cie_lch: return normalizedFromLab(labFromLch(v));
        default: return normalizedFromXyz(xyz());
    }
}

// The hub -> a CIE or Oklab model's colors.
Vector3 cieFromHub(ColorFamily family, Hub hub, const Vector3& t) noexcept
{
    if (hub == Hub::same) {
        return t;
    }
    if (hub == Hub::lab) {
        return family == ColorFamily::cie_lch ? lchFromLab(t) : t;
    }
    const auto fromXyz = [&](const Vector3& xyz) {
        switch (family) {
            case ColorFamily::cie_xyy: return xyyFromXyz(xyz);
            case ColorFamily::cie_yuv: return yuvFromXyz(xyz);
            case ColorFamily::oklab: return oklabFromXyz(xyz);
            default: return xyz;
        }
    };
    if (hub == Hub::xyz) {
        return fromXyz(t);
    }
    switch (family) {
        case ColorFamily::cie_lab: return labFromNormalized(t);
        case ColorFamily::cie_lch: return lchFromLab(labFromNormalized(t));
        default: return fromXyz(xyzFromNormalized(t));
    }
}

std::shared_ptr<const ConversionPlan> build(const FormatDescriptor& source, const FormatDescriptor& destination)
{
    const ModelDescriptor& from = *source.model;
    const ModelDescriptor& to = *destination.model;
    const SpaceDescriptor& from_space = *source.space;
    const SpaceDescriptor& to_space = *destination.space;
    const bool from_rgb = from.rgbLike(); // RGB and Y'CbCr
    const bool to_rgb = to.rgbLike();
    const bool to_gray = to.family == ColorFamily::gray;
    const bool from_cie = !from.rgbOrGray();
    const bool to_cie = !to.rgbOrGray();

    ConversionPlan::Steps steps{};
    for (std::size_t i = 0; i < source.component_count; ++i) {
        steps.unpack[i] = unpackFor(*source.components[i].type);
        if (steps.unpack[i] == nullptr) {
            return nullptr;
        }
    }
    for (std::size_t i = 0; i < destination.component_count; ++i) {
        steps.pack[i] = packFor(*destination.components[i].type);
        if (steps.pack[i] == nullptr) {
            return nullptr;
        }
    }
    for (std::size_t index = 0; index < from.component_count; ++index) {
        bool stored = false;
        for (std::size_t i = 0; i < source.component_count; ++i) {
            stored = stored || source.components[i].model_index == index;
        }
        if (!stored) {
            steps.fill_index[steps.fill_count++] = index;
        }
    }

    // The hub between the models, and the linear steps between hubs (N7). In a
    // space derived from chromaticities R = G = B is the D50 white, so a gray
    // source needs no matrix: in normalized XYZ it is (Y, Y, Y), in XYZ
    // (Y Xn, Y, Y Zn). Matrix spaces are the exception below (N9). The
    // normalized hub serves Lab and LCH, whose a* and b* of a gray source are
    // then 0 exactly; without them XYZ itself saves the division and
    // multiplication by the white. RGB -> gray takes the luminance in the source
    // space; XYZ -> gray is Y.
    const auto labLike = [](ColorFamily f) { return f == ColorFamily::cie_lab || f == ColorFamily::cie_lch; };
    if (from_cie && to_cie && from.family == to.family) {
        steps.hub = Hub::same;
    } else if (from_cie && to_cie && labLike(from.family) && labLike(to.family)) {
        steps.hub = Hub::lab;
    } else if (from_cie || to_cie) {
        steps.hub = labLike(from.family) || labLike(to.family) ? Hub::normalized : Hub::xyz;
    } else {
        steps.hub = Hub::rgb;
    }
    const bool normalized = steps.hub == Hub::normalized;
    // The neutral shortcuts rest on R = G = B mapping to the D50 white, which
    // spaces derived from chromaticities guarantee by construction. A space made
    // from a caller's matrix does not (N9): there gray goes through the matrices
    // like the RGB neutral it is, and RGB neutrals take no shortcut to CIE.
    const bool from_gray = from.family == ColorFamily::gray;
    const bool same_matrix = from_space.rgb_to_xyz == to_space.rgb_to_xyz;
    const bool gray_as_rgb = from_gray && (from_space.from_matrix || (to_rgb && to_space.from_matrix));
    const bool rgb_source = from_rgb || gray_as_rgb;
    if (rgb_source && to_rgb) {
        steps.apply_matrix = !same_matrix;
        if (steps.apply_matrix) {
            steps.matrix = multiply(to_space.xyz_to_rgb, from_space.rgb_to_xyz);
        }
    } else if (rgb_source && to_cie) {
        steps.apply_matrix = true; // RGB -> XYZ; for normalized XYZ, rows divided by the white
        for (std::size_t r = 0; r < 3; ++r) {
            for (std::size_t k = 0; k < 3; ++k) {
                steps.matrix[r][k] = normalized ? from_space.rgb_to_xyz[r][k] / cie_white[r] : from_space.rgb_to_xyz[r][k];
            }
        }
    } else if (from_cie && to_rgb) {
        steps.apply_matrix = true; // XYZ -> RGB; from normalized XYZ, columns multiplied by the white
        for (std::size_t r = 0; r < 3; ++r) {
            for (std::size_t k = 0; k < 3; ++k) {
                steps.matrix[r][k] = normalized ? to_space.xyz_to_rgb[r][k] * cie_white[k] : to_space.xyz_to_rgb[r][k];
            }
        }
    }
    // Gray to gray within one matrix keeps its value; between matrices it is the
    // source luminance of R = G = B.
    steps.luminance = (from_rgb || (gray_as_rgb && !same_matrix)) && to_gray;
    steps.weights = from_space.luminance();
    steps.take_y = from_cie && to_gray;
    // A gray value is R = G = B in its own space (N11): a luminance (XYZ Y)
    // divided by the luminance of R = G = B = 1, which is exactly 1 in a space
    // derived from chromaticities and the Y-row sum in a matrix space. With a sum
    // of 0 a luminance cannot determine the gray value (NaN). Gray to gray within
    // one matrix keeps its value.
    steps.divide_luminance = to_gray && to_space.from_matrix && !(from_gray && same_matrix);
    const Vector3& y_row = to_space.luminance();
    const double unit_luminance = y_row[0] + y_row[1] + y_row[2];
    steps.luminance_divisor = unit_luminance != 0.0 ? unit_luminance : std::numeric_limits<double>::quiet_NaN();
    steps.neutral_to_cie = !from_cie && to_cie && !from_space.from_matrix;
    // LCH formats store every component in one type. Rounding to binary32 can
    // take a hue just below 360 to 360, or a chroma to 0, so the hue rules are
    // applied again to the values as stored. That includes copies from LCH
    // double, so a direct conversion equals one staged through LCH double.
    steps.binary32_hue = to.family == ColorFamily::cie_lch && destination.uniform_type != nullptr &&
                         destination.uniform_type->type == ComponentType::f32;

    // Colors stay encoded when no linear-light operation is needed and both
    // sides apply the same TRC to every channel: TRCs act per channel, so
    // copying or replicating encoded values is exact, while decode + encode
    // would only add rounding (or, for an extreme gamma, underflow or overflow
    // double; keeping the encoded value is then the better result). The
    // exception is a gamma TRC, which decodes values <= 0 to 0: a model change
    // keeps that clamp. Within one model and space only the type changes, and
    // values are kept. CIE models have no TRC.
    bool same_trcs = true;
    for (std::size_t c = 0; c < to.colorCount(); ++c) {
        same_trcs = same_trcs && trcOf(from, from_space, from_rgb ? c : 0) == trcOf(to, to_space, c);
    }
    const bool stay_encoded = same_trcs && !steps.apply_matrix && !steps.luminance && !steps.divide_luminance;
    const bool type_only = &from == &to && &from_space == &to_space;
    bool any_clamp = false;
    for (std::size_t c = 0; c < 3; ++c) {
        const TrcDescriptor* source_trc = c < from.colorCount() ? trcOf(from, from_space, c) : nullptr;
        steps.clamp_encoded[c] = stay_encoded && !type_only && source_trc != nullptr &&
                                 source_trc->kind == TransferFunction::Kind::gamma;
        any_clamp = any_clamp || steps.clamp_encoded[c];
        steps.decode[c] = stay_encoded ? nullptr : trcOf(from, from_space, c);
        steps.encode[c] = stay_encoded ? nullptr : trcOf(to, to_space, c);
    }
    // Association (scaling colors by alpha) commutes with every step between
    // decode and encode, so two associated models in the same domain keep it.
    // A clamp needs straight values: the used alpha can be negative. For a
    // finite alpha, dividing and multiplying again is the identity. A NaN or
    // infinite alpha turns colors into NaN, which a model change must keep
    // (a type-only conversion copies). CIE models have straight alpha only.
    const bool any_trc = steps.decode[0] != nullptr || steps.encode[0] != nullptr || any_clamp;
    const bool keep_association = from.associated() && to.associated() && !any_trc;
    steps.unassociate = from.associated() && !keep_association;
    steps.associate = to.associated() && !keep_association;
    steps.nonfinite_alpha_to_nan = keep_association && !type_only;
    // Y'CbCr converts from and to R~G~B~ of its space (S4). Between two formats
    // of one Y'CbCr model whose values stay encoded the two steps cancel, so the
    // values are copied.
    const auto ycbcrOf = [](const ModelDescriptor& m) -> const YcbcrCoefficients* {
        return m.family == ColorFamily::ycbcr      ? &bt601_ycbcr
               : m.family == ColorFamily::ycbcr709 ? &bt709_ycbcr
                                                   : nullptr;
    };
    const bool same_ycbcr = from.family == to.family && stay_encoded;
    steps.source_ycbcr = same_ycbcr ? nullptr : ycbcrOf(from);
    steps.destination_ycbcr = same_ycbcr ? nullptr : ycbcrOf(to);
    return std::make_shared<const ConversionPlan>(source, destination, steps);
}

} // namespace

ConversionPlan::ConversionPlan(const FormatDescriptor& source, const FormatDescriptor& destination,
                               const Steps& steps) noexcept
    : source_(&source),
      destination_(&destination),
      copy_(&source == &destination),
      steps_(steps)
{
}

std::shared_ptr<const ConversionPlan> ConversionPlan::get(const FormatDescriptor& source,
                                                          const FormatDescriptor& destination)
{
    // Immortal, like the registries: calls from atexit handlers or static
    // destructors may still create plans.
    struct Cache {
        std::mutex mutex;
        std::map<std::pair<const FormatDescriptor*, const FormatDescriptor*>, std::shared_ptr<const ConversionPlan>>
            plans;
    };
    static Cache* const cache = new Cache;

    const std::lock_guard lock(cache->mutex);
    if (const auto found = cache->plans.find({&source, &destination}); found != cache->plans.end()) {
        return found->second;
    }
    // Build, then publish: if building or inserting throws (std::bad_alloc),
    // nothing is cached and the next call tries again.
    std::shared_ptr<const ConversionPlan> plan = build(source, destination);
    cache->plans.emplace(std::pair{&source, &destination}, plan);
    return plan;
}

void ConversionPlan::convertModels(const double* in, double* out, std::size_t pixels) const noexcept
{
    const ModelDescriptor& from = *source_->model;
    const ModelDescriptor& to = *destination_->model;
    const std::size_t in_stride = from.component_count;
    const std::size_t out_stride = to.component_count;
    const std::size_t in_colors = from.colorCount();
    const std::size_t out_colors = to.colorCount();
    const bool from_rgb_or_gray = from.rgbOrGray();
    const bool to_rgb_or_gray = to.rgbOrGray();
    const Vector3& w = steps_.weights;
    const Matrix3& m = steps_.matrix;

    for (std::size_t p = 0; p < pixels; ++p) {
        const double* src = in + p * in_stride;
        double* dst = out + p * out_stride;
        const double alpha = from.hasAlpha() ? src[in_colors] : 1.0;

        // Source colors to the hub.
        Vector3 c{};
        bool neutral = false;
        if (from_rgb_or_gray) {
            // Y'CbCr to R~G~B~ first; un-associate in the source's own domain,
            // then decode.
            Vector3 encoded{};
            for (std::size_t i = 0; i < in_colors; ++i) {
                encoded[i] = src[i];
            }
            if (steps_.source_ycbcr != nullptr) {
                encoded = rgbFromYcbcr(encoded, *steps_.source_ycbcr);
            }
            const double source_scale = steps_.unassociate ? usedAlpha(alpha) : 1.0;
            for (std::size_t i = 0; i < in_colors; ++i) {
                c[i] = steps_.unassociate ? encoded[i] / source_scale : encoded[i];
                if (steps_.clamp_encoded[i] && c[i] <= 0.0) {
                    c[i] = 0.0; // as decode + encode would; NaN passes
                }
                if (steps_.decode[i] != nullptr) {
                    c[i] = toLinear(*steps_.decode[i], c[i]);
                }
            }
            // To a CIE model a neutral needs no matrix where R = G = B is the D50
            // white (spaces derived from chromaticities, N9): it is (v, v, v) in
            // normalized XYZ. That holds for gray (N7) and for RGB with equal
            // decoded channels (N8).
            neutral = steps_.neutral_to_cie && (in_colors == 1 || (c[0] == c[1] && c[1] == c[2]));
            if (in_colors == 1) {
                c[1] = c[2] = c[0]; // gray is R = G = B
            }
            if (neutral && steps_.hub == Hub::xyz) {
                c = xyzFromNormalized(c);
            }
        } else {
            c = cieToHub(from.family, steps_.hub, {src[0], src[1], src[2]});
        }

        // Between the hubs: model and space changes in linear light.
        Vector3 v{};
        if (out_colors == 1) {
            v[0] = steps_.luminance ? (w[0] * c[0] + w[1] * c[1] + w[2] * c[2]) : (steps_.take_y ? c[1] : c[0]);
            if (steps_.divide_luminance) {
                v[0] /= steps_.luminance_divisor;
            }
        } else if (steps_.apply_matrix && !neutral) {
            for (std::size_t r = 0; r < 3; ++r) {
                v[r] = m[r][0] * c[0] + m[r][1] * c[1] + m[r][2] * c[2];
            }
        } else {
            v = c;
        }

        // The hub to destination colors.
        if (to_rgb_or_gray) {
            // Encode, then associate in the destination's domain.
            double destination_scale = 1.0;
            if (steps_.associate) {
                destination_scale = usedAlpha(alpha);
            } else if (steps_.nonfinite_alpha_to_nan) {
                // Kept association: exactly 1 for finite alpha, NaN for NaN or +-Inf.
                destination_scale = usedAlpha(alpha) / usedAlpha(alpha);
            }
            for (std::size_t i = 0; i < out_colors; ++i) {
                dst[i] = v[i];
                if (steps_.encode[i] != nullptr) {
                    dst[i] = fromLinear(*steps_.encode[i], dst[i]);
                }
                if (steps_.associate || steps_.nonfinite_alpha_to_nan) {
                    dst[i] *= destination_scale;
                }
            }
            if (steps_.destination_ycbcr != nullptr) { // R~G~B~ to Y'CbCr last
                const Vector3 ycc = ycbcrFromRgb({dst[0], dst[1], dst[2]}, *steps_.destination_ycbcr);
                dst[0] = ycc[0];
                dst[1] = ycc[1];
                dst[2] = ycc[2];
            }
        } else {
            Vector3 colors = cieFromHub(to.family, steps_.hub, v);
            if (steps_.binary32_hue && (narrowToFloat(colors[1]) == 0.0f || narrowToFloat(colors[2]) == 360.0f)) {
                colors[2] = 0.0;
            }
            dst[0] = colors[0];
            dst[1] = colors[1];
            dst[2] = colors[2];
        }
        if (to.hasAlpha()) {
            dst[out_colors] = alpha;
        }
    }
}

void ConversionPlan::run(const std::byte* source, std::byte* destination, std::size_t pixel_count) const noexcept
{
    const std::size_t source_stride = source_->bytesPerPixel();
    const std::size_t destination_stride = destination_->bytesPerPixel();
    if (copy_) {
        std::memcpy(destination, source, pixel_count * source_stride);
        return;
    }

    const ModelDescriptor& from = *source_->model;
    const ModelDescriptor& to = *destination_->model;
    double unpacked[chunk_pixels * max_components];
    double converted[chunk_pixels * max_components];
    for (std::size_t done = 0; done < pixel_count;) {
        const std::size_t pixels = std::min(chunk_pixels, pixel_count - done);
        const std::byte* in = source + done * source_stride;
        std::byte* out = destination + done * destination_stride;

        // Unpack to doubles in model order.
        if (const TypeDescriptor* type = source_->uniform_type) {
            steps_.unpack[0](*type, in, type->size, unpacked, 1, pixels * from.component_count);
        } else {
            std::size_t offset = 0;
            for (std::size_t i = 0; i < source_->component_count; ++i) {
                const FormatComponent& component = source_->components[i];
                steps_.unpack[i](*component.type, in + offset, source_stride, unpacked + component.model_index,
                                 from.component_count, pixels);
                offset += component.type->size;
            }
            for (std::size_t f = 0; f < steps_.fill_count; ++f) {
                const std::size_t index = steps_.fill_index[f];
                const double value = from.hasAlpha() && index == from.color_count ? 1.0 : 0.0;
                for (std::size_t p = 0; p < pixels; ++p) {
                    unpacked[p * from.component_count + index] = value;
                }
            }
        }

        convertModels(unpacked, converted, pixels);

        // Pack the stored destination components.
        if (const TypeDescriptor* type = destination_->uniform_type) {
            steps_.pack[0](*type, converted, 1, out, type->size, pixels * to.component_count);
        } else {
            std::size_t offset = 0;
            for (std::size_t i = 0; i < destination_->component_count; ++i) {
                const FormatComponent& component = destination_->components[i];
                steps_.pack[i](*component.type, converted + component.model_index, to.component_count,
                               out + offset, destination_stride, pixels);
                offset += component.type->size;
            }
        }
        done += pixels;
    }
}

} // namespace chromaroute::detail
