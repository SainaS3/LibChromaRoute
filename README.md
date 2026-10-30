# LibChromaRoute

A C++20 static library for converting pixels between formats and color spaces.

- **Formats:** 134 built in, each available in every color space.
  - 108 RGB and gray formats: 18 models × 6 types. Models can be linear, `'` (the
    space's TRC) or `~` (the sRGB curve), with no alpha, straight alpha or associated
    alpha. Examples: `RGBA`, `R'aG'aB'aA`, `R~G~B~`, `Y'A`.
  - 26 CIE formats: `CIE XYZ`, `CIE xyY`, `CIE Yuv`, `CIE Lab` and `CIE LCH(ab)`, each
    with or without alpha, in `float` and `double`; plus `CIE Lab u8/u16` (ICC v4
    Lab encodings) and `CIE L float`.
  - More color models follow in later milestones ([docs/model_coverage.md](docs/model_coverage.md)).
- **Types:** `u8`, `u16`, `u32`, `half`, `float`, `double`.
- **Color spaces:** sRGB, scRGB, Rec2020, DisplayP3, Adobish, ProPhoto, Apple,
  ACEScg, ACES2065-1, plus spaces created from chromaticities or an RGB to XYZ matrix
  with linear, sRGB or gamma TRCs. A format in another space is named
  `"R'G'B'A u8-DisplayP3"`.
- **API:** `PixelFormat`, `ColorSpace`, `TransferFunction`, `Converter`. Every pair of
  formats converts.
- **Verified** against independent high-precision oracles, on the desktop and inside
  a UWP AppContainer. See [docs/testing.md](docs/testing.md).

```cpp
#include <chromaroute/chromaroute.hpp>

auto source = chromaroute::PixelFormat::fromName("R'G'B'A u8");
auto target = chromaroute::PixelFormat::fromName("RGBA float");
auto converter = chromaroute::Converter::create(*source, *target);
chromaroute::Status status = converter->process(std::as_bytes(std::span(pixels)),
                                                std::as_writable_bytes(std::span(linear)), pixel_count);
```

## Not supported

- ICC profiles (planned for M8), rendering intents, black-point compensation and
  gamut mapping. Integer outputs clamp each channel on its own.
- Absolute luminance and HDR transfer functions (PQ, HLG). The built-in Rec2020 uses
  the sRGB curve and ProPhoto a pure 1.8 power, as in babl, not BT.2020's OETF or
  ProPhoto's ROMM curve (open decision S6).
- Color models other than RGB, gray and CIE: Oklab, Y′CbCr, HSL/HSV/HCY and CMYK
  follow in M5–M7.
- Buffers other than tightly packed interleaved pixels: no row stride (convert one
  row at a time), planar or subsampled data, BGRA/BGR or packed formats, or in-place
  conversion.
- Associated alpha keeps color at alpha 0 by dividing and multiplying by 2^-16 there,
  which is not ordinary premultiplication (open decision N10).
- iOS and Mac Catalyst builds, an install package and benchmarks.

## Targets

1. **Windows UWP x64 and ARM64**, MSVC: current.
2. iOS (device and simulator), arm64: later.
3. Mac Catalyst: later.

## Build

Requirements: Visual Studio 2026 with the UWP workload (C++ UWP tools), a Windows 10/11
SDK, CMake ≥ 4.2 (the first release with the Visual Studio 18 2026 generator that every
preset uses), and Windows Developer Mode (for the UWP runtime tests).

```
cmake --list-presets=all
cmake --workflow --preset msvc-uwp-x64-debug      # configure, build, run UWP runtime tests
cmake --workflow --preset msvc-uwp-x64-release
cmake --workflow --preset msvc-uwp-arm64-debug    # same for ARM64 (run tests on an ARM64 host)
cmake --workflow --preset msvc-uwp-arm64-release
```

Individual steps: `cmake --preset msvc-uwp-x64-debug`,
`cmake --build --preset msvc-uwp-x64-debug`, `ctest --preset msvc-uwp-x64-debug`.

`msvc-host-x64-*` and `msvc-host-arm64-*` presets build a plain desktop test runner for fast iteration. They
are for development only and are not a deliverable.

If the checkout is on a drive without ownership metadata (exFAT), git needs
`git config --global --add safe.directory <path>`.

## Documentation

| Document | Contents |
| --- | --- |
| [docs/architecture.md](docs/architecture.md) | Layout, core model, conversion engine, API, numeric rules |
| [docs/decisions.md](docs/decisions.md) | Design decisions and their status |
| [docs/testing.md](docs/testing.md) | Test layers, oracles, UWP harness |
| [docs/environment.md](docs/environment.md) | Toolchain and build findings |

## License

Not yet chosen.
