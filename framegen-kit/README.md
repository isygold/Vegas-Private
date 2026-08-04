# VEGAS Frame Generation Kit

3-pass motion-compensated frame generation for DXVK-based projects targeting
Qualcomm Adreno GPUs on Android emulation (Star Emulator / Winlator).

## ⚠️ Disclaimer

This frame generation implementation is provided **for testing and evaluation
purposes only**. Enabling frame generation alongside other interpolation or
upscaling features (including the emulator's own FSR, LSFG-VK, or bionic
framegen) may produce compounding artifacts, increased latency, or unexpected
behavior. The authors of this kit assume **no responsibility for any loss,
damage, game crashes, or degraded experience** that may result from using this
code. By using this kit you accept full responsibility for testing and
evaluating it in your own environment.

## What This Is

Frame generation (sometimes called "motion interpolation") synthesises an
in-between frame between two consecutively rendered frames. It uses motion
estimation to track how pixels move from frame N to frame N+1, then
interpolates to produce frame N+0.5. The result is smoother motion at the
same rendering cost — the GPU still renders at the native framerate, but the
display shows twice as many frames. Think of it as the mobile equivalent of
DLSS 3 Frame Generation.

**Frame generation does NOT increase FPS.** The GPU still renders at the
native framerate. FG increases the *displayed* framerate by showing interpolated
frames, which makes motion appear smoother. For actual FPS gains, use FSR
upscaling or the TBDR-aware governor.

### 3-Pass Pipeline

| Pass | What it does |
|------|-------------|
| **Motion estimation** | Block SAD on prev/cur frames to compute per-block motion vectors |
| **Median filter** | 3×3 spatial denoise on the motion field to remove outliers |
| **Warp + blend** | Warp the previous frame by the filtered motion, alpha-blend at 0.5 |

The entire pipeline runs as compute shaders on the GPU — no pixel shader
overhead, no geometry cost.

## What's Included

```
framegen-kit/
├── README.md              ← You are here
├── LICENSE                ← zlib/libpng (see below)
├── src/
│   ├── framegen.h         ← Standalone Framegen class declaration
│   │                         (framegen-only: no governor, no FSR, no HUD)
│   └── framegen.cpp       ← Standalone Framegen implementation
│                              (1828 lines, verbatim from dxvk_vegas.cpp)
├── shaders/
│   └── star_fg_motion_v2.comp  ← GLSL compute shader source (motion pass;
│                                  median + warp are in separate compiled
│                                  pipelines embedded in star_fg_spv.h)
└── spirv/
    ├── fg_motion.spv      ← Motion estimation SPIR-V binary
    ├── fg_motion.spvasm   ← Motion estimation SPIR-V assembly
    ├── fg_median.spv      ← Median filter SPIR-V binary
    ├── fg_median.spvasm   ← Median filter SPIR-V assembly
    ├── fg_warp.spv        ← Warp + blend SPIR-V binary
    ├── fg_warp.spvasm     ← Warp + blend SPIR-V assembly
    ├── fg_motion_v2.spv   ← Motion estimation v2 SPIR-V binary
    └── fg_motion_v2.spvasm← Motion estimation v2 SPIR-V assembly
```

## Integration (DXVK)

### Prerequisites

- A DXVK-based project (VEGAS fork, or your own DXVK derivative)
- Vulkan 1.3+ support on the target GPU
- Mesa 25.x+ Turnip driver on Android (recommended)
- The `Tristate` enum from your `dxvk_options.h` (standard in DXVK)

### Step 1: Copy the module into your project

Copy `src/framegen.h` and `src/framegen.cpp` into your DXVK source tree.
They are self-contained: the module owns its own Vulkan function table,
intermediate images, and dispatch logic.

### Step 2: Provide the host integration hooks

The module expects the host to call these before any framegen dispatch:

```cpp
// At device init (once):
Framegen::init(
    device,                    // VkDevice
    queue,                     // VkQueue (graphics queue)
    queueFamily,               // uint32_t queue family index
    physicalDevice);           // VkPhysicalDevice

// Each frame (or when config changes):
Framegen::setConfig(
    dxvk::Tristate::Auto);     // or True/False

Framegen::setSmoothFrameTimeMs(
    smoothedFrameTimeMs);      // float, ms — governor EMA proxy
```

The `smoothedFrameTimeMs` value is used by the adaptive blend policy
(controls how aggressively framegen blends vs. shows the current frame).
The original VEGAS code reads this from the governor's EMA (`s_gov.smoothFrameTimeMs`).
If your project has no governor, use the raw frame time or a simple EMA.

### Step 3: Wire the framegen gate into your swapchain present path

In your D3D11, D3D9, and DXGI swapchain `PresentImage()` methods, add the
framegen check before the present call:

```cpp
// Evaluate framegen eligibility (Tier 2/3 + headroom + ready).
// Tier 1 (Adreno 610/619) is excluded by design — compute budget.
m_needsFrameGen = Framegen::isFrameGenReady()
  && Framegen::needsFrameGen(frameTimeMs, Vegas::getTier());
```

Then, after the normal present, dispatch framegen if eligible:

```cpp
if (m_needsFrameGen) {
  Framegen::dispatch(
    curImage, prevImage, extent, format,
    hudRectValid, hudRect);
}
```

### Step 4: Add the `vegas.enableFramegen` config entry to `dxvk.conf`

```ini
# Frame generation: Auto (tier-gated), True (force), False (disable)
vegas.enableFramegen = Auto
```

### Step 5: Update the HUD (optional)

The framegen dispatch is compatible with the standard DXVK HUD. The
`vegas` HUD token shows VEGAS branding and tier info. The legacy `cpu`
token (per-core CPU load) has been removed from the HUD in the current
build — it performed `/proc` + `/sys` reads at 1 Hz and added a noisy
row.

Valid VEGAS HUD tokens: `devinfo`, `fps`, `frametimes`, `gpuload`,
`vegas`, `version`, `commit`.

## API Reference

### `Framegen::needsFrameGen(float frameTimeMs, uint32_t tier)`

Returns `true` if framegen should activate this frame.

- **Auto mode** (default): Tier 1 → `false`. Tier 2 → `frameTime <= 29ms`.
  Tier 3 → `frameTime <= 33ms`.
- **True mode**: always returns `true` (force-enable regardless of tier).
- **False mode**: handled at `isFrameGenReady()` level (returns `false`).

### `Framegen::isFrameGenReady()`

Returns `true` if the frame generator has a valid `VkDevice`/`VkQueue` and
the user toggle is not `False`.

### `Framegen::dispatch(curImage, prevImage, extent, format,
hudRectValid, hudRect)`

Dispatches the 3-pass framegen pipeline. Returns `true` on successful
dispatch, `false` if skipped (first frame, saturated queue, unsupported
format, or user disabled).

- `curImage` — current rendered frame (`VK_IMAGE_LAYOUT_GENERAL`)
- `prevImage` — previous frame (`VK_IMAGE_LAYOUT_GENERAL`)
- `extent` — image dimensions
- `format` — must be `VK_FORMAT_R8G8B8A8_UNORM` or `B8G8R8A8_UNORM`
- `hudRectValid` — `true` if `hudRect` holds a valid HUD graph rect
- `hudRect` — `[x0, y0, x1, y1]` in WSI pixels; HUD region is exempted
  from framegen (avoids warping the HUD graph)

### `Framegen::outputImage()`

Returns the `VkImage` handle (as `uint64_t`) of the interpolated output
frame. Note: `dispatch()` blits the output to `curImage` internally,
so this getter is for debug/inspection only.

### `Framegen::isActive()`

Returns `true` if the frame generator is currently active (used for HUD
metrics).

### `Framegen::drain()`

Drains any in-flight async FG work (blocks until GPU completes).
Safe to call before swapchain resize. Idempotent.

## Compatibility Notes

### Bionic Frame Generation

If you're adapting this for a project that already has a bionic framegen
implementation, note that the VEGAS framegen uses a **3-pass compute-only**
pipeline (no pixel shader involvement). The motion estimation pass uses
block SAD on 16×16 tiles, which is different from bionic's optical flow
approach. The warp+blend pass alpha-blends at 0.5, which is the standard
for motion-compensated interpolation.

Key differences from bionic LSFG:
- VEGAS uses block SAD (fast, deterministic) vs. bionic's feature-matching
- VEGAS is compute-only (no RT dependencies) vs. bionic's potential RT
  readback
- VEGAS has an adaptive skip window (FG_SKIP_WINDOW = 60 frames) when the
  GPU queue is saturated, falling back to native present

### LSFG (Light Speed Frame Generation)

LSFG (AMD's Light Speed Frame Generation) is a similar motion-compensated
approach. The VEGAS framegen is architecturally compatible with the LSFG
pattern (motion → median → warp+blend) but uses different heuristics for
eligibility and a different tile-based SAD implementation tuned for Adreno
TBDR GPUs.

If you're porting to a non-Adreno GPU, the tier gate (`needsFrameGen`
returns `false` for tier 1) and the headroom check (`frameTime <= 29ms`
for tier 2, `<= 33ms` for tier 3) should be replaced with GPU-appropriate
thresholds.

### Combined with FSR

FSR (spatial upscaler) and framegen (temporal interpolator) can coexist.
The recommended order: **FSR first (spatial), then framegen (temporal)**.
FSR renders at lower resolution and upscales; framegen interpolates between
the upscaled frames. Running both produces the smoothest result at the
lowest rendering cost.

## Build Notes

- The module is self-contained: it owns its own Vulkan function table
  (`FgVulkanFuncs`), intermediate images, and command pools.
- The SPIR-V shaders are embedded as bytecode arrays in `star_fg_spv.h` —
  no runtime file I/O for shaders.
- The framegen dispatch uses a dedicated command pool and fence for
  async compute. Resources from timed-out dispatches are parked in a
  pending list (`s_fgPending`) and cleaned up on the next dispatch.
- Telemetry is gated behind the `vegas_telemetry=1` environment variable
  (off by default).
- The module does **not** link against DXVK or any other library at the
  C++ level — only Vulkan headers and standard C++ library.

## License

This code is licensed under the **zlib/libpng license** (see `LICENSE` file).
The underlying DXVK base is also MIT/zlib licensed. See the LICENSE file
for full terms.

## Credits

- **Lead Developer:** isygold
- **Framegen Implementation:** isygold (VEGAS fork)
- **Framegen Testing:** @devaspe
- **Base Project:** DXVK v2.4.1 by doitsujin
- **License:** zlib/libpng
