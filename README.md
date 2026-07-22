<p align="center">
  <img alt="VEGAS" src="./vegas_banner.gif" width="80%">
  <br>
  <em>star engine rebased and rebranded; vegas</em>
</p>

# VEGAS — DXVK 2.4.1
### Stability Backport: GPLAsync + Performance Optimizations for Adreno Mobile

VEGAS is a specialized performance fork of **DXVK v2.4.1** (via the GPLAsync backport) targeting **Qualcomm Adreno GPUs** on Android emulation (Star Emulator / Winlator). It features automatic async shader compilation, tier-based auto-tuning, FSR 1.0 compute upscaling, motion-compensated frame generation, and a TBDR-aware dynamic governor — all configurable through simple DXVK options.

**This is the stable backport branch (`build-fix-2.4.1`).** The feature branch (`2.7.4-beta`) contains the full DXVK v2.7.3 base with GPU BCn-to-ASTC transcoding — use this branch for maximum stability and compatibility.

---

## Key Features

### GPLAsync — Async Shader Compilation (Backported)
Eliminates shader compilation stutter by compiling pipelines asynchronously in background worker threads:
- **`dxvk.enableAsync = true`** (default ON) — pipelines compile in background, return fast-link fallback immediately
- **`dxvk.gplAsyncCache = false`** (default OFF) — opt-in state cache with GPL fixes
- Environment overrides: `DXVK_ASYNC=0` to disable, `DXVK_GPLASYNCCACHE=1` to enable cache
- Render-target frame tracking: only enables async after 5+ consecutive frames of RT binding (prevents missing geometry from unloaded shaders)

### Tier-Based Auto-Tuning
Adreno GPUs are classified into 3 tiers from the KGSL device model:

| Tier | Adreno GPUs | Draw Threshold | HAAE Threshold | Cap Multiplier |
|------|-------------|----------------|----------------|----------------|
| 1 | 506-620 (low-end) | 50 | 30 | 1.5x |
| 2 | 630-690 (mid) | 150 | 50 | 1.8x |
| 3 | 7xx/8xx (high-end) | 300 | 100 | 2.5x |

Each tier receives tuned draw thresholds, HAAE pacing, frame generation eligibility, and governor cap multipliers automatically. Low thresholds ({50,150,300}) ensure 2D games like Hollow Knight flush draw batches promptly — no pop-in or missing geometry.

### FSR 1.0 Compute Upscaler
Full FSR 1.0 EASU compute pipeline with async dispatch via timeline semaphore:
- Upscales when render resolution is below swapchain resolution
- Configurable per-game behavior
- Eliminates 0.5-1.0ms CPU stall on every upscaled frame

### 3-Pass Motion-Compensated Frame Generation
Available on Tier 2 (<= 29ms frametime) and Tier 3 (<= 33ms frametime):
1. Motion estimation — block SAD on prev/cur frames
2. Median filter — 3x3 spatial denoise on motion field
3. Warp + blend — warp prev frame by filtered motion, alpha-blend at 0.5

Compute-only pipeline. Disabled on Tier 1 (insufficient compute budget).

### Adaptive Governor — TBDR-Inverted
EMA-smoothed frame-time telemetry with adaptive cooldown:
- **GPU-bound** (load>0.85, ft>20ms) — raise threshold (batch more, amortize overhead)
- **CPU-bound** (load<0.45, ft>10ms) — lower threshold (flush earlier, TBDR tile pacing)
- **Balanced** — reset to base
- **Adaptive cooldown:** `ceil(ft x 0.3)`, clamped [5,30] frames

**Why inverted?** Desktop DXVK raises thresholds for both CPU-bound and GPU-bound scenarios. On TBDR Adreno, raising the threshold when CPU-bound makes the problem worse — more draws accumulate in the tile buffer. The inverted path correctly reduces the threshold to force earlier flushes.

### VegaHud Performance Overlay
- Lightweight performance HUD with frame-skip optimization (updates every 5th frame)
- Draw call count, frame time, GPU load, tier info
- Configurable via standard `DXVK_HUD` environment variable

### Dynamic VRAM & GPU Mask
- VRAM clamped to ~40% of system RAM (1-4 GB range)
- Adreno tier mapped to compatible NVIDIA vendor/device ID for game compatibility

---

## Installation

### WCP Package Types
Each release provides **two** WCP packages with identical DLLs but different metadata:

| Package | Type field | For |
|---------|-----------|-----|
| `dxvk-2.4.1-vegas-*.wcp` | DXVK | Stock Winlator and general Android DXVK use |
| `vegas-2.4.1-*.wcp` | VEGAS | Star Emulator (latest build) |

### Via Star Emulator
1. Open Star Emulator
2. Go to **Contents** menu
3. Install the `vegas-2.4.1-*.wcp` package (VEGAS-native type)

### Via Stock Winlator
1. Download the `dxvk-2.4.1-vegas-*.wcp` package
2. Install it as a standard DXVK WCP package in Winlator

### Manual Configuration
Place `dxvk.conf` in any of these paths:
- `/storage/emulated/0/Winlator/`
- `/storage/emulated/0/Download/`
- `/storage/emulated/0/`

Or set `DXVK_CONFIG_FILE` to your config path.

---

## Configuration

```ini
# Master switch: Auto (Adreno only), True (force-on), False (force-off)
dxvk.enableStarProfile = Auto

# Async shader compilation: True (default), False
dxvk.enableAsync = True

# GPL async state cache: False (default), True
dxvk.gplAsyncCache = False

# Manual tier override (advanced): 0=auto, 1=low-end, 2=mid, 3=high-end
vegas.forceTier = 0

# Compiler thread count (advanced): 0=auto (max 4 on ARM64)
dxvk.numCompilerThreads = 0

# Environment variable overrides:
# DXVK_ASYNC=0         → disable async compilation
# DXVK_GPLASYNCCACHE=1 → enable GPL state cache
```

All other parameters (thresholds, bind skip, HAAE pacing, quality scaling) are auto-tuned by the VEGAS engine.

---

## Build from Source

```bash
git clone --recursive https://github.com/isygold/Vegas-Private.git
cd Vegas-Private
git checkout build-fix-2.4.1

# Android cross-build (requires NDK r26+ and Meson 0.58+)
meson setup --cross-file build-android-aarch64.txt \
  --buildtype release --prefix /output/dir build
cd build
ninja install
```

The output DLLs (`d3d9.dll`, `d3d11.dll`, `dxgi.dll`, etc.) are placed in `/output/dir/bin/`.

---

## Changelog (VEGAS 2.4.1)

| Commit | Feature |
|--------|---------|
| `38cab11` | **VEGAS branding:** project name 'vegas', dirty suffix '-1-vegas' |
| `d404649` | **HK fix:** draw thresholds lowered to {50,150,300} — fixes 2D game pop-in |
| `fff1bec` | **HUD branding:** shows "VEGAS" instead of "DXVK" |
| `9bd1c38` | **Perf batch:** framegen timeout (50ms), C1-C4 optimizations |
| `97c53a4` | **GPLAsync backport:** async shader compilation for DXVK 2.4.1 |
| `60bd0f0` | **WCP security:** permissions tightened, no external pushes |
| `439305a` | **WCP artifact-based:** fetch from build artifacts not releases |
| `1a2b491` | **Build-only workflow:** strip release/WCP from build.yml |

### Performance Optimizations (C1-C4)
- **C1 — FSR ratio guard:** skip FSR if source >= 85% of target (waste check)
- **C2 — Governor re-tune:** GPU-bound at load>0.85/ft>20ms, CPU-bound at load<0.45/ft>10ms, adaptive cooldown
- **C3 — HUD frame-skip:** pushMetrics() writes every 5th call via thread_local counter
- **C4 — Threshold tuning:** draw {50,150,300}, HAAE {30,50,100}

---

## Notes

- **This is a backport, not a VEGAS upgrade path.** VEGAS 2.4.1 is a backport of GPLAsync + VEGAS performance features onto DXVK 2.4.1. It is NOT an upgrade from an earlier VEGAS version — it is a separate stable branch. The feature branch (`2.7.4-beta`) with the full DXVK v2.7.3 base and GPU transcoder is a different track.
- **Faster than stock DXVK and plain GPLAsync.** The combination of async shader compilation (GPLAsync), TBDR-aware governor, FSR upscaling, and low-latency draw thresholds makes VEGAS 2.4.1 faster and smoother than both stock DXVK 2.4.1 and standalone dxvk-gplasync builds. Users upgrading from either will see measurable improvements.
- **Tier 1 (Adreno 5xx/6xx low-end):** Frame generation disabled. FSR available but not recommended at very low resolutions. Zero-init enabled for Turnip stability.
- **Turnip driver:** Use Mesa 25.x+ with Vulkan 1.3 support for best results.
- **Synthetic benchmarks:** May show lower FPS than stock due to draw thresholds. Judge performance by actual gameplay smoothness.
- **GPU-bound workloads:** VSync-off provides negligible gain when the GPU is already saturated (17+ ms frame times).
- **Draw thresholds {50,150,300} are intentionally low** to fix 2D game pop-in/missing geometry (e.g., Hollow Knight). This does NOT affect rendering correctness — every draw call still renders, just with more frequent flushes.

---

## Upstream DXVK Reference

For desktop/Wine usage, driver notes, HUD reference, debugging, and full build instructions, see the [upstream DXVK README](https://github.com/doitsujin/dxvk). Key highlights preserved below:

### HUD (Android)
`DXVK_HUD=devinfo,fps` or `DXVK_HUD=full` in your container environment. Common options:
- `devinfo` — GPU name + driver version
- `fps` — Current frame rate
- `frametimes` — Frame time graph
- `gpuload` — Estimated GPU load
- `compiler` — Shader compiler activity
- `drawcalls` — Draw calls per frame

### Debugging (Android)
- `DXVK_LOG_LEVEL=warn` — Reduce log verbosity
- `DXVK_LOG_LEVEL=debug` — Verbose logging for troubleshooting
- `DXVK_CONFIG="dxgi.syncInterval = 0"` — Set config via environment variable

### Device Filter
`DXVK_FILTER_DEVICE_NAME="Adreno"` to select a specific Vulkan device if multiple GPUs are present.

### Anti-Cheat Warning
Modifying Direct3D libraries in multiplayer games may result in account bans. **Use at your own risk.**

---

## Credits

- **Lead Developer:** isygold
- **Base Project:** DXVK v2.4.1 by doitsujin
- **GPLAsync Patch:** Ph42oN (dxvk-gplasync v2.4-1), ishitatsuyuki (upstream GPLAsync)
- **FSR 1.0:** AMD GPUOpen (EASU compute shader)
- **License:** zlib/libpng
