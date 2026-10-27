# LibChromaRoute

A C++20 static library for converting pixels between formats and color spaces.

- **Formats:** 108 built in (18 RGB and gray models x 6 types), each available in
  every color space. Models can be linear, `'` (the space's TRC) or `~` (the sRGB
  curve), with no alpha, straight alpha or associated alpha. Examples: `RGBA`,
  `R'aG'aB'aA`, `R~G~B~`, `Y'A`.
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

## Targets

1. **Windows UWP x64**, MSVC: current.
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
```

Individual steps: `cmake --preset msvc-uwp-x64-debug`,
`cmake --build --preset msvc-uwp-x64-debug`, `ctest --preset msvc-uwp-x64-debug`.

`msvc-host-x64-*` presets build a plain desktop test runner for fast iteration. They
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
