# VEGAS Frame Generation Kit

3-pass motion-compensated frame generation for DXVK-based projects targeting
Qualcomm Adreno GPUs on Android emulation (Star Emulator / Winlator).

## What This Is

Frame generation (sometimes called "motion interpolation") synthesises an
in-between frame between two consecutively rendered frames. It uses motion
estimation to track how pixels move from frame N to frame N+1, then
interpolates to produce frame N+0.5. The result is smoother motion at the
same rendering cost — the GPU still renders at the native framerate, but the
display shows twice as many frames. Think of it as the mobile equivalent of
DLSS 3 Frame Generation.

### 3-Pass Pipeline

| Pass | What it does |
|------|-------------|
| **Motion estimation** | Block SAD on prev/cur frames to compute per-block motion vectors |
| **Median filter** | 3×3 spatial denoise on the motion field to remove outliers |
| **Warp + blend** | Warp the previous frame by the filtered motion, alpha-blend at 0.5 |

The entire pipeline runs as compute shaders on the GPU — no pixel shader
overhead, no geometry cost.

## Does Frame Generation Increase FPS?

**No.** Frame generation does not make the GPU render faster. It makes
the display show more frames per second by synthesising in-between
frames. This is the single most common misconception about framegen,
and it's worth understanding clearly.

### What actually happens

| Metric | Without FG | With FG |
|--------|-----------|---------|
| GPU render cost | 33ms/frame | 33ms/frame + ~2–4ms FG compute |
| Displayed framerate | 30fps | 60fps |
| GPU rendering throughput | 30 frames/sec | 30 frames/sec (unchanged) |
| Motion smoothness | Juddery | Smooth |

The GPU still renders 30 frames per second. Framegen just gives the
display 60 frames to show — 30 real, 30 interpolated. The FPS counter
in games and emulators counts **displayed frames**, so it reads 60fps.
That's real — the screen is genuinely showing 60 frames per second.
But the GPU didn't get faster; it's doing the same work it was doing
before.

### Why it looks like a big FPS jump

The human eye is extremely sensitive to motion smoothness. A 30fps
game with judder looks choppy. The same game at 60fps (even half of
those frames being synthetic) looks buttery smooth. That's why framegen
feels like a huge upgrade — it is, perceptually. But it's not a
rendering throughput gain.

### The benchmark trap

Benchmarkers sometimes report "60fps with framegen" as if it's a
performance win. It isn't — it's a presentation win. The GPU didn't
render 60 frames; the display is showing 60 frames, half of which are
synthetic. A real FPS increase means the GPU renders 60 frames per
second natively, which requires twice the GPU power.

### What actually increases FPS

If you want more FPS, these are the features that deliver real rendering
throughput gains:

- **FSR 1.0 upscaling** — renders at lower resolution, upscales output
- **TBDR-aware governor** — better batching, fewer wasted draw calls
- **Bind-skip** — skips redundant pipeline binds
- **Async shader compilation** — eliminates stutter (not raw FPS)

Framegen is a **smoothness feature**, not a performance feature. It
makes your game look better at the same rendering cost.

## What's Included

```
framegen-kit/
├── README.md              ← You are here
├── LICENSE                ← zlib/libpng (see below)
├── src/
│   ├── dxvk_vegas.cpp     ← Full VEGAS implementation (framegen functions:
│   │                         needsFrameGen, isFrameGenReady, framegenDispatch,
│   │                         initFgPipeline, ensureFgIntermediateImages,
│   │                         fgDrainPending, fgCleanup, plus telemetry & stats)
│   ├── dxvk_vegas.h       ← Framegen class declarations (needsFrameGen,
│   │                         isFrameGenReady, framegenDispatch, framegenOutputImage,
│   │                         plus all static member declarations)
│   └── star_fg_spv.h      ← SPIR-V bytecode embedding header (auto-generated;
│                             do not edit manually — rebuild from .comp source)
├── shaders/
│   └── star_fg_motion_v2.comp  ← GLSL compute shader source (motion estimation
│                                  pass; median + warp are in separate compiled
│                                  pipelines embedded in star_fg_spv.h)
└── spirv/
    ├── fg_motion.spv      ← Motion estimation SPIR-V binary
    ├── fg_motion.spvasm   ← Motion estimation SPIR-V assembly (disassembly)
    ├── fg_median.spv      ← Median filter SPIR-V binary
    ├── fg_median.spvasm   ← Median filter SPIR-V assembly
    ├── fg_warp.spv        ← Warp + blend SPIR-V binary
    ├── fg_warp.spvasm     ← Warp + blend SPIR-V assembly
    ├── fg_motion_v2.spv   ← Motion estimation v2 SPIR-V binary (updated variant)
    └── fg_motion_v2.spvasm← Motion estimation v2 SPIR-V assembly
```

## Integration (DXVK)

### Prerequisites

- A DXVK-based project (VEGAS fork, or your own DXVK derivative)
- Vulkan 1.3+ support on the target GPU
- Mesa 25.x+ Turnip driver on Android (recommended)
- The `Tristate` enum available in your `dxvk_options.h` (standard in DXVK)

### Step 1: Add the config option

In your `dxvk_options.h`, add the framegen toggle next to the other VEGAS
options:

```cpp
/// Frame generation toggle (Auto = tier+headroom gate,
/// True = force enable regardless of tier, False = disable)
Tristate vegasEnableFramegen = Tristate::Auto;
```

In your `dxvk_options.cpp`, parse it:

```cpp
vegasEnableFramegen = config.getOption<Tristate>("vegas.enableFramegen",
                                                  Tristate::Auto);
```

### Step 2: Add the framegen gate to your swapchain present path

In your D3D11, D3D9, and DXGI swapchain `PresentImage()` methods, add the
framegen check before the present call:

```cpp
// Evaluate framegen eligibility (Tier 2/3 + headroom + ready).
m_needsFrameGen = Vegas::isFrameGenReady()
  && Vegas::needsFrameGen(frameTime, Vegas::getTier());
```

Then, after the normal present, dispatch framegen if eligible:

```cpp
if (m_needsFrameGen) {
  Vegas::framegenDispatch(
    curImage, prevImage, extent, format,
    hudRectValid, hudRect);
}
```

### Step 3: Wire the static device pointer

The framegen code uses a static `DxvkDevice*` pointer (`s_dxvkDevice`) to
access per-device config. Ensure this is set during device initialisation:

```cpp
// In your device init or configure() path:
Vegas::s_dxvkDevice = device;
```

### Step 4: Add the SPIR-V shader pipelines

The framegen dispatch creates three compute pipelines (motion, median, warp)
from the SPIR-V bytecode embedded in `star_fg_spv.h`. Ensure the bytecode
arrays (`dxvk_fg_motion_code`, `dxvk_fg_median_code`, `dxvk_fg_warp_code`)
are linked into your build. These are generated from the `.comp` shader
source via `glslangValidator → spirv-as → spirv-dis` and embedded as
`static const uint32_t[]` arrays in `star_fg_spv.h`.

To rebuild the SPIR-V from source:

```bash
glslangValidator -V shaders/star_fg_motion_v2.comp -o spirv/fg_motion.spv
spirv-as spirv/fg_motion.spv -o spirv/fg_motion.spvasm
# Repeat for median and warp passes (separate .comp files)
```

### Step 5: Add the `vegas.enableFramegen` config entry to `dxvk.conf`

```ini
# Frame generation: Auto (tier-gated), True (force), False (disable)
vegas.enableFramegen = Auto
```

### Step 6: Update the HUD (optional)

The framegen dispatch is compatible with the standard DXVK HUD. The
`vegas` HUD token shows VEGAS branding and tier info. The legacy `cpu`
token (per-core CPU load) has been removed from the HUD in the current
build — it performed `/proc` + `/sys` reads at 1 Hz and added a noisy row.

Valid VEGAS HUD tokens: `devinfo`, `fps`, `frametimes`, `gpuload`,
`vegas`, `version`, `commit`.

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

## API Reference

### `Vegas::needsFrameGen(float frameTime, uint32_t tier)`

Returns `true` if framegen should activate this frame.

- **Auto mode**: Tier 1 → `false`. Tier 2 → `frameTime <= 29ms`.
  Tier 3 → `frameTime <= 33ms`.
- **True mode**: Always returns `true` (force-enable regardless of tier).
- **False mode**: Handled at `isFrameGenReady()` level (returns `false`).

### `Vegas::isFrameGenReady()`

Returns `true` if the frame generator has a valid `VkDevice`/`VkQueue` and
the user toggle is not `False`.

### `Vegas::framegenDispatch(curImage, prevImage, extent, format,
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

### `Vegas::framegenOutputImage()`

Returns the `VkImage` handle (as `uint64_t`) of the interpolated output
frame. Note: `framegenDispatch` blits the output to `curImage` internally,
so this getter is for debug/inspection only.

## Build Notes

- The framegen code is compiled into the same DLL as the rest of VEGAS
  (no separate library).
- The SPIR-V shaders are embedded as bytecode arrays in `star_fg_spv.h` —
  no runtime file I/O for shaders.
- The framegen dispatch uses a dedicated command pool and fence for
  async compute. Resources from timed-out dispatches are parked in a
  pending list (`s_fgPending`) and cleaned up on the next dispatch.
- Telemetry is gated behind the `vegas_telemetry=1` environment variable
  (off by default).

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
