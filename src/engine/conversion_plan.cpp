#include "engine/conversion_plan.hpp"

#include "engine/color_math.hpp"
#include "engine/half.hpp"
#include "engine/numeric.hpp"
#include "engine/transfer.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>

namespace chromaroute::detail {
namespace {

// Pixels per pipeline pass; the double scratch buffers live on the stack.
constexpr std::size_t chunk_pixels = 128;
constexpr std::size_t max_components = 4;

template <typename Storage>
Storage load(const std::byte* in, std::size_t index) noexcept
{
    Storage value;
    std::memcpy(&value, in + index * sizeof value, sizeof value); // buffers may be unaligned
    return value;
}

template <typename Storage>
void store(std::byte* out, std::size_t index, Storage value) noexcept
{
    std::memcpy(out + index * sizeof value, &value, sizeof value);
}

template <typename UInt>
void unpackUnorm(const std::byte* in, double* out, std::size_t values)
{
    for (std::size_t i = 0; i < values; ++i) {
        out[i] = decodeUnorm(load<UInt>(in, i));
    }
}

template <typename UInt>
void packUnorm(const double* in, std::byte* out, std::size_t values)
{
    for (std::size_t i = 0; i < values; ++i) {
        store(out, i, encodeUnorm<UInt>(in[i]));
    }
}

void unpackHalf(const std::byte* in, double* out, std::size_t values)
{
    for (std::size_t i = 0; i < values; ++i) {
        out[i] = decodeHalf(load<std::uint16_t>(in, i));
    }
}

void packHalf(const double* in, std::byte* out, std::size_t values)
{
    for (std::size_t i = 0; i < values; ++i) {
        store(out, i, encodeHalf(in[i]));
    }
}

void unpackF32(const std::byte* in, double* out, std::size_t values)
{
    for (std::size_t i = 0; i < values; ++i) {
        out[i] = load<float>(in, i);
    }
}

void packF32(const double* in, std::byte* out, std::size_t values)
{
    for (std::size_t i = 0; i < values; ++i) {
        store(out, i, narrowToFloat(in[i]));
    }
}

void unpackF64(const std::byte* in, double* out, std::size_t values)
{
    for (std::size_t i = 0; i < values; ++i) {
        out[i] = load<double>(in, i);
    }
}

void packF64(const double* in, std::byte* out, std::size_t values)
{
    for (std::size_t i = 0; i < values; ++i) {
        store(out, i, in[i]);
    }
}

ConversionPlan::UnpackFunction unpackFor(ComponentType type)
{
    switch (type) {
        case ComponentType::u8: return unpackUnorm<std::uint8_t>;
        case ComponentType::u16: return unpackUnorm<std::uint16_t>;
        case ComponentType::u32: return unpackUnorm<std::uint32_t>;
        case ComponentType::f16: return unpackHalf;
        case ComponentType::f32: return unpackF32;
        case ComponentType::f64: return unpackF64;
    }
    return nullptr;
}

ConversionPlan::PackFunction packFor(ComponentType type)
{
    switch (type) {
        case ComponentType::u8: return packUnorm<std::uint8_t>;
        case ComponentType::u16: return packUnorm<std::uint16_t>;
        case ComponentType::u32: return packUnorm<std::uint32_t>;
        case ComponentType::f16: return packHalf;
        case ComponentType::f32: return packF32;
        case ComponentType::f64: return packF64;
    }
    return nullptr;
}

// The TRC a model applies to channel c in a space; nullptr for linear models.
// Gray models use the space's first (red) TRC, perceptual models always sRGB.
const TrcDescriptor* trcOf(const ModelDescriptor& model, const SpaceDescriptor& space, std::size_t channel)
{
    switch (model.encoding) {
        case Encoding::linear: return nullptr;
        case Encoding::space_trc: return space.trc[model.family == ColorFamily::gray ? 0 : channel];
        case Encoding::perceptual: return &srgbTrc();
    }
    return nullptr;
}

std::shared_ptr<const ConversionPlan> build(const FormatDescriptor& source, const FormatDescriptor& destination)
{
    if (source.model->component_count > max_components || destination.model->component_count > max_components) {
        return nullptr;
    }
    const ModelDescriptor& from = *source.model;
    const ModelDescriptor& to = *destination.model;
    const SpaceDescriptor& from_space = *source.space;
    const SpaceDescriptor& to_space = *destination.space;
    const bool from_rgb = from.family == ColorFamily::rgb;
    const bool to_rgb = to.family == ColorFamily::rgb;

    ConversionPlan::Steps steps{};
    steps.unpack = unpackFor(source.type->type);
    steps.pack = packFor(destination.type->type);
    if (steps.unpack == nullptr || steps.pack == nullptr) {
        return nullptr;
    }
    // Gray is neutral in every space (every matrix maps R = G = B to the D50
    // white), so only RGB -> RGB needs the matrix; luminance (XYZ Y) is taken in
    // the source space.
    steps.apply_matrix = from_rgb && to_rgb && from_space.rgb_to_xyz != to_space.rgb_to_xyz;
    if (steps.apply_matrix) {
        steps.matrix = multiply(to_space.xyz_to_rgb, from_space.rgb_to_xyz);
    }
    steps.luminance = from_rgb && !to_rgb;
    steps.weights = from_space.luminance();

    // Colors stay encoded when no linear-light operation is needed and both
    // sides apply the same TRC to every channel: TRCs act per channel, so
    // copying or replicating encoded values is exact, while decode + encode
    // would only add rounding (or, for an extreme gamma, underflow or overflow
    // double; keeping the encoded value is then the better result). The
    // exception is a gamma TRC, which decodes values <= 0 to 0: a model change
    // keeps that clamp. Within one model and space only the type changes, and
    // values are kept.
    bool same_trcs = true;
    for (std::size_t c = 0; c < to.colorCount(); ++c) {
        same_trcs = same_trcs && trcOf(from, from_space, from_rgb ? c : 0) == trcOf(to, to_space, c);
    }
    const bool stay_encoded = same_trcs && !steps.apply_matrix && !steps.luminance;
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
    // (a type-only conversion copies).
    const bool any_trc = steps.decode[0] != nullptr || steps.encode[0] != nullptr || any_clamp;
    const bool keep_association = from.associated() && to.associated() && !any_trc;
    steps.unassociate = from.associated() && !keep_association;
    steps.associate = to.associated() && !keep_association;
    steps.nonfinite_alpha_to_nan = keep_association && !type_only;
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
    const Vector3& w = steps_.weights;
    const Matrix3& m = steps_.matrix;

    for (std::size_t p = 0; p < pixels; ++p) {
        const double* src = in + p * in_stride;
        double* dst = out + p * out_stride;
        const double alpha = from.hasAlpha() ? src[in_colors] : 1.0;

        // Source colors: un-associate in the source's own domain, then decode.
        double c[3];
        const double source_scale = steps_.unassociate ? usedAlpha(alpha) : 1.0;
        for (std::size_t i = 0; i < in_colors; ++i) {
            c[i] = steps_.unassociate ? src[i] / source_scale : src[i];
            if (steps_.clamp_encoded[i] && c[i] <= 0.0) {
                c[i] = 0.0; // as decode + encode would; NaN passes
            }
            if (steps_.decode[i] != nullptr) {
                c[i] = toLinear(*steps_.decode[i], c[i]);
            }
        }
        if (in_colors == 1) {
            c[1] = c[2] = c[0]; // gray is neutral in every space
        }

        // Linear-light model and space changes.
        if (out_colors == 1) {
            dst[0] = steps_.luminance ? (w[0] * c[0] + w[1] * c[1] + w[2] * c[2]) : c[0];
        } else if (steps_.apply_matrix) {
            for (std::size_t r = 0; r < 3; ++r) {
                dst[r] = m[r][0] * c[0] + m[r][1] * c[1] + m[r][2] * c[2];
            }
        } else {
            dst[0] = c[0];
            dst[1] = c[1];
            dst[2] = c[2];
        }

        // Destination colors: encode, then associate in the destination's domain.
        double destination_scale = 1.0;
        if (steps_.associate) {
            destination_scale = usedAlpha(alpha);
        } else if (steps_.nonfinite_alpha_to_nan) {
            // Kept association: exactly 1 for finite alpha, NaN for NaN or +-Inf.
            destination_scale = usedAlpha(alpha) / usedAlpha(alpha);
        }
        for (std::size_t i = 0; i < out_colors; ++i) {
            if (steps_.encode[i] != nullptr) {
                dst[i] = fromLinear(*steps_.encode[i], dst[i]);
            }
            if (steps_.associate || steps_.nonfinite_alpha_to_nan) {
                dst[i] *= destination_scale;
            }
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

    const std::size_t in_components = source_->model->component_count;
    const std::size_t out_components = destination_->model->component_count;
    double unpacked[chunk_pixels * max_components];
    double converted[chunk_pixels * max_components];
    for (std::size_t done = 0; done < pixel_count;) {
        const std::size_t pixels = std::min(chunk_pixels, pixel_count - done);
        steps_.unpack(source + done * source_stride, unpacked, pixels * in_components);
        convertModels(unpacked, converted, pixels);
        steps_.pack(converted, destination + done * destination_stride, pixels * out_components);
        done += pixels;
    }
}

} // namespace chromaroute::detail
