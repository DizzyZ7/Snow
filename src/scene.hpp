#pragma once
#include <cstdint>
#include <vector>

namespace snow {
inline constexpr std::size_t MaxParticles = 4096;
inline constexpr std::size_t MaxSurfaces = 128;
inline constexpr std::size_t MaxSurfaceCells = 2048;
inline constexpr std::size_t MaxDrawItems = 32768;
inline constexpr int CellWidth = 8;
struct Rect {
    int left{}, top{}, right{}, bottom{};
    [[nodiscard]] int width() const noexcept { return right - left; }
    [[nodiscard]] int height() const noexcept { return bottom - top; }
    [[nodiscard]] bool valid() const noexcept { return right > left && bottom > top; }
    bool operator==(const Rect&) const = default;
};
struct Span {
    int left{}, right{};
    bool operator==(const Span&) const = default;
};
struct SurfaceSpec {
    std::uint64_t id{}; // tracker generation, never the HWND alone
    Rect bounds{}; // top is the supporting horizontal edge
    bool taskbar{};
    std::vector<Span> visible; // absolute physical x coordinates, already z-order clipped
};
// x/y are the top-left of a quad in physical desktop coordinates.
// Color is straight RGBA; the renderer emits premultiplied alpha.
// shape: 0 rectangle, 1 antialiased ellipse, 2 six-arm snow crystal. No textures or external assets.
struct DrawItem {
    float x{}, y{}, w{}, h{};
    float r{1}, g{1}, b{1}, a{1};
    float shape{};
};
}
