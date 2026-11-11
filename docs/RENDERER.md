# Renderer baseline and candidate

GDI remains the default. `D3D11` is an explicit experimental selection, pending
measurements and interactive Windows validation. Choosing a GPU API does not
establish a lower process working set or lower total memory use.

## Integration contract

`src/renderer.hpp` exposes `snow::RendererKind { Gdi, D3D11 }` and a movable,
noncopyable `snow::Renderer` with:

```cpp
static DWORD windowStyles(RendererKind kind) noexcept;
bool initialize(HWND hwnd, Rect monitor, RendererKind kind);
bool resize(Rect monitor);
bool render(std::span<const DrawItem> items);
void release() noexcept;
const wchar_t* name() const noexcept;
std::uint64_t cpuBufferBytes() const noexcept;
std::uint64_t presentCount() const noexcept;
```

Use one renderer and caller-owned `WS_POPUP` window per physical monitor. Create
the window with `Renderer::windowStyles(kind)`, a window class without
`CS_OWNDC`/`CS_CLASSDC`, no background brush, and no child windows. Initialize,
resize, render, release and destroy on its owning UI thread. The caller shows
the overlay without activation after its initial frame and destroys the HWND
after releasing the renderer. The renderer positions/resizes the HWND but does
not own or destroy it. In normal mode, the window procedure should also return
`HTTRANSPARENT` for `WM_NCHITTEST` and `MA_NOACTIVATE` for `WM_MOUSEACTIVATE`.

Coordinates are absolute physical desktop pixels; a monitor may have a negative
origin. Each renderer clips the scene to its own monitor. Items are processed in
input order with source-over alpha. Colors are straight RGBA in the scene and
premultiplied only during rendering. Invalid/nonfinite geometry or color,
nonpositive dimensions, zero/negative alpha and invisible items are ignored.
Valid colors are clamped to `[0,1]`. A shape value of at least `0.5` selects an
antialiased ellipse; other values select a rectangle. Empty scenes clear the
previous frame. Shapes use procedural coverage and have no texture assets.

The maximum is `MaxDrawItems` (32,768) per submitted scene. Monitors must have
positive extents, dimensions at most 16,384 and at most 33,554,432 pixels. Width
and height validation uses 64-bit subtraction before narrowing; malformed
extents cannot wrap into allocations. The application must separately cap its
monitor count. Per-monitor allocation avoids virtual-desktop gaps.

`initialize` first releases the old renderer. Invalid resize dimensions and
over-capacity scenes return `false` without presenting or allocating, retaining
an existing healthy renderer. Native allocation, resize and present failures
return `false` and release resources. There is no device-loss spin/retry loop:
the application decides whether to recreate once, delay retry, fall back to GDI
or stop displaying an overlay. Repeated initialization is explicit. GDI frame
counting and DXGI frame counting indicate successful native submissions, not
proof that a frame was visible or displayed at a particular time. `release`
resets counters and reported bytes and is safe to call repeatedly.

Recreate the HWND when changing renderer kind, especially D3D11 to GDI.
`SetLayeredWindowAttributes` is used for the candidate and prevents subsequent
`UpdateLayeredWindow` calls until the `WS_EX_LAYERED` style is cleared and set
again. Recreating also makes the two different style sets explicit. Release
does not hide or repaint the caller's window; the application hides/destroys it
when stopping an overlay.

Required renderer link libraries are `user32`, `gdi32`, `d3d11`, `dxgi`, `dcomp`
and `d3dcompiler`. The smoke executable additionally links `dwmapi`. Build with
Windows 10/11 SDK headers, C++20, `UNICODE`, `_UNICODE`, `WIN32_LEAN_AND_MEAN`
and `NOMINMAX`.

## GDI baseline

GDI owns one persistent top-down 32-bit BGRA DIB and one compatible memory DC
per monitor. Creation and resize restore the selected bitmap before deleting
the bitmap/DC; no per-frame brush, pen, bitmap or DC is created. Each frame
clears this image, rasterizes the clipped scene directly into its pixels with
premultiplied source-over blending, then calls `UpdateLayeredWindow` with
`ULW_ALPHA` and `AC_SRC_ALPHA`. The alpha channel is written explicitly; GDI's
`Ellipse`/`FillRect` alpha behavior is not relied upon. Fractional rectangle
edges and ellipse edges get procedural antialiasing.

This retains the simple GDI architecture of the MVP while replacing its color
key and per-frame brush allocation. It still clears and submits a full monitor
image every frame, so CPU work/bandwidth must be measured. Resizing frees the
old DIB before allocating its replacement, avoiding a temporary second image.

## D3D11/DirectComposition candidate

The candidate requires hardware D3D11 feature level 11.0. It compiles embedded
HLSL once at initialization, creates a composition swap chain with two BGRA8
buffers (`FLIP_SEQUENTIAL`, `STRETCH`, `PREMULTIPLIED`), attaches one visual and
target, and renders procedural instanced six-vertex quads. A pixel shader
computes ellipse coverage and premultiplies color. The blend state uses
`ONE / INV_SRC_ALPHA` for both color and alpha. There is no depth buffer,
texture atlas, full-frame CPU image or explicit GPU-to-CPU readback.

One persistent dynamic instance buffer holds 32,768 64-byte instances (2 MiB).
Visible items are written straight into its mapped storage with
`WRITE_DISCARD`; no per-frame `std::vector` or D3D resource is created. Clipped
quad positions and original-shape UVs preserve ellipses across monitor edges.
One 16-byte target constant buffer is updated per frame. DXGI maximum frame
latency is set to one, and a single `Present(1,0)` is issued per render call.
Presentation can block for composition/vsync; the application must measure its
impact across multiple monitors. Drivers can allocate/rename backing storage;
the declared logical payload is not a bound on driver-managed memory.

Both style sets include `WS_EX_LAYERED | WS_EX_TRANSPARENT |
WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST`. The candidate also includes
`WS_EX_NOREDIRECTIONBITMAP` to avoid an opaque GDI redirection surface and sets
layered opacity to 255, without supplying a CPU image. The layered-transparent
combination is deliberate: `WS_EX_TRANSPARENT` on an ordinary window describes
paint ordering and should not alone be treated as proof of cross-process
click-through. Microsoft documents mouse bypass for **layered** transparent
windows and permits layered HWNDs as DirectComposition targets. The combined
layered/no-redirection/DComp behavior remains unverified in this implementation
until interactive Windows tests confirm visible alpha, mouse pass-through to
another process, keyboard focus, touch/pen behavior and tool-mode transitions.
`HTTRANSPARENT` alone only delegates hit testing to windows on the same thread.
The candidate must remain opt-in; failure to render correctly is grounds to
retain GDI on that environment.

## Allocation sizing versus measurements

The following is arithmetic for logical image payloads, **not measured RAM**.
OS compositor surfaces, D3D device/driver state, allocation alignment, upload
renaming, shared GPU memory and the rest of the application are excluded.

| Per-monitor size | GDI image (`w*h*4`) | D3D two swap buffers (`w*h*8`) | D3D instance/constant payload |
| --- | ---: | ---: | ---: |
| 1920 x 1080 | 7.91 MiB | 15.82 MiB | 2 MiB + 16 B |
| 3840 x 2160 | 31.64 MiB | 63.28 MiB | 2 MiB + 16 B |
| 7680 x 4320 | 126.56 MiB | 253.13 MiB | 2 MiB + 16 B |

`cpuBufferBytes()` reports the logical CPU-writable image/upload capacity: the
DIB bytes for GDI, or instance/constant payload for D3D. It excludes shader
compiler temporaries and all OS/driver overhead, and is not a process memory
measurement. An upper accepted GDI target needs 128 MiB for its one image; an
upper accepted D3D target needs 256 MiB just for two image payloads, plus upload
and device resources. A global sub-40-MiB promise cannot be inferred from this
design, especially for 4K/multiple monitors. A separate device is currently
created per monitor; shared-device savings are a possible later measurement
driven change.

Before changing the default, collect the same release build, scene seed, snow
quantity, weather, frame rate and monitor geometry for both renderer choices.
Record Windows/driver/GPU/CPU details, process private bytes and working set,
GPU dedicated/shared use, CPU utilization, GPU activity, presentation latency
and frame-time percentiles. Sample steady rendering, pause/resume, repeated
topology/DPI changes, display sleep, Explorer restart and device-loss recovery.
Keep initial allocation, steady state and eight-hour trends separate. Resource
stability in a short smoke test does not establish eight-hour stability.

## Verification and remaining evidence

`tests/renderer_smoke.cpp` was written before the interface. Its first syntax
compile failed on the absent `renderer.hpp`, as expected. After implementation,
the Linux syntax compile stops at missing `windows.h`: there is no Windows SDK
or native Windows execution in this environment. Neither renderer is claimed
to have compiled or run here. Windows CI and interactive results must be
recorded separately after execution.

The default Windows smoke exercises real HWND/GDI resources: alpha rectangle
and ellipse submissions, malformed/offscreen items, 120 repeated presentations,
resizes including negative origins, actual HWND position, stable CPU capacity
and GDI handle counts, oversized inputs/overflowing extents, empty scene clear,
idempotent release, caller-owned HWND lifetime and release after a failed native
present. Successful submissions do not themselves validate displayed pixels.

Run `renderer_smoke --visual` only on an unlocked SDR interactive desktop; it
creates its own blue backdrop, uses `DwmFlush`, reads composited display pixels
and checks half-transparent red over blue plus clear pixels. HDR/color
management or unrelated topmost windows can affect the display sample and must
be recorded with the result. The default smoke prints this pixel check as
`NOT RUN`.

Run `renderer_smoke --d3d` to explicitly exercise the candidate's lifecycle.
Unavailable initialization prints `SKIP` and exits **77**; configure
`SKIP_RETURN_CODE 77` on an optional CTest entry. It never reports a skipped
candidate as passing. `--d3d --visual` requires a working candidate and checks
both paths' display alpha; an unavailable/broken visual candidate fails. The
default smoke prints D3D as `NOT RUN`. Actual cross-process mouse/keyboard,
multi-DPI, remote-desktop, touch/pen and eight-hour checks remain manual; no
automated smoke result substitutes for them.

## Microsoft references

- [Layered windows and hit testing](https://learn.microsoft.com/en-us/windows/win32/winmsg/window-features#layered-windows)
- [Extended window styles](https://learn.microsoft.com/en-us/windows/win32/winmsg/extended-window-styles)
- [WM_NCHITTEST and HTTRANSPARENT thread restriction](https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-nchittest)
- [DirectComposition HWND target, including layered HWNDs](https://learn.microsoft.com/en-us/windows/win32/api/dcomp/nf-dcomp-idcompositiondevice-createtargetforhwnd)
- [Composition swap-chain requirements](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforcomposition)
- [SetLayeredWindowAttributes and UpdateLayeredWindow restriction](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setlayeredwindowattributes)
- [UpdateLayeredWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-updatelayeredwindow)
