#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <windows.h>

#include "scene.hpp"

namespace snow {
enum class RendererKind { Gdi, D3D11 };

// One renderer and one caller-owned WS_POPUP HWND per physical monitor.
// All methods, including destruction, run on that HWND's owning UI thread.
class Renderer final {
public:
    Renderer() noexcept;
    ~Renderer();
    Renderer(Renderer&&) noexcept;
    Renderer& operator=(Renderer&&) noexcept;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    [[nodiscard]] static DWORD windowStyles(RendererKind kind) noexcept;
    [[nodiscard]] bool initialize(HWND hwnd, Rect monitor, RendererKind kind);
    [[nodiscard]] bool resize(Rect monitor);
    // Items use absolute physical desktop coordinates and straight RGBA.
    // Exact shape codes: 0 rectangle, 1 ellipse, 2 six-arm snow crystal.
    // The crystal is intended for foreground flakes at least 6 physical pixels wide.
    // Other shape values, including fractional codes, are ignored deterministically.
    // Malformed/invisible items are ignored; an over-capacity span is rejected.
    [[nodiscard]] bool render(std::span<const DrawItem> items);
    void release() noexcept;

    [[nodiscard]] const wchar_t* name() const noexcept;
    // Logical CPU-writable image/upload capacity, excluding OS/driver overhead.
    [[nodiscard]] std::uint64_t cpuBufferBytes() const noexcept;
    // Successful native submissions since initialize(); reset by release().
    [[nodiscard]] std::uint64_t presentCount() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
