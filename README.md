# 🎰 VEGAS: DXVK v2.7.2.1
### **Adreno-Tuned DXVK Fork for Android Emulation (Star Emulator / Winlator)**

**VEGAS** (formerly Star Engine) is a specialized performance modification of DXVK designed for **Qualcomm Adreno GPUs** in mobile environments. It brings a tier-based auto-tuning engine, FSR 1.0 compute upscaling, motion-compensated frame generation, and dynamic driver safety features — all controlled by a single master switch.

---

## 🎯 Key Features

### 🧠 Star Profile Master Switch (`dxvk.enableStarProfile`)
All Vegas features are gated behind one Tristate option:
- **Auto** — Enable on Adreno GPUs, disable on all others (default)
- **True** — Force-enable all features (for testing on non-Adreno)
- **False** — Hard-disable everything (emergency escape hatch)

### 🏎️ Tier-Based Auto-Tuning
Adreno GPUs are classified into 3 tiers via sysfs (`/sys/class/kgsl/kgsl-3d0/gpu_model`):

| Tier | GPUs | Persona |
|------|------|---------|
| 1 | Adreno 610, 619 | GTX 1050 Ti |
| 2 | Adreno 640, 642L, 650, 660 | GTX 1070 |
| 3 | Adreno 7xx, 8xx | RTX 3060 |

Each tier gets tuned draw thresholds, frame gen eligibility, HAAE quality scaling, and VRAM budgets automatically.

### 🔬 FSR 1.0 Compute Upscaler (`vegas.enableUpscaler`)
Full FSR 1.0 EASU compute pipeline:
- **Auto** — Upscale when render resolution < swapchain resolution
- **True** — Always upscale (half-resolution quadrants)
- **False** — Disabled

Uses push constants + 2-binding descriptor set, dispatch with blit + fence sync.

### 🎞️ 3-Pass Motion-Compensated Frame Generation
Available on Tier 2 (≤29ms frametime) and Tier 3 (≤33ms frametime):
1. **Motion estimation** — Block SAD on prev/cur frames → raw motion vectors
2. **Median filter** — 3×3 spatial denoise on motion field
3. **Warp + blend** — Warp prev frame by filtered motion, alpha-blend at weight 0.5

Compute-only pipeline with shared 4-binding descriptor layout.

### 🛡️ Adaptive Governor (`tuneThreshold`)
EMA-smoothed frame-time telemetry with 120-frame cooldown prevents threshold oscillation. Automatically adjusts mid-frame flush threshold based on GPU load.

### 📊 Dynamic VRAM & GPU Mask
- `applyVramSwap(Config&)` — Sets `dxgi.maxDeviceMemory` to 40% of system RAM (clamped 1–4 GB)
- `applyGpuMask(Config&)` — Maps Adreno tier to NVIDIA vendor/device ID for game compatibility

### 🧹 Bind Skip Optimization
Skips redundant `vkCmdBindPipeline` calls when no dynamic state has changed — reduces CPU overhead.

### 🎨 HUD Performance Colors
Graph coloring via `getGraphColor()`: green (normal) → yellow (lagging) → orange (stuttering) → red (overheating).

---

## 🛠️ Installation

### Via Star Emulator
1. Open Star Emulator
2. Navigate to **Contents** menu
3. Install the `dxvk-2.7.2.1.wcp` file

### Manual Setup
Place `dxvk.conf` in any of these paths:
- `/storage/emulated/0/Winlator/`
- `/storage/emulated/0/Download/`
- `/storage/emulated/0/`

Or set `DXVK_CONFIG_FILE` env var to your config path.

---

## ⚙️ Configuration

```ini
# Master switch: Auto (Adreno only), True (force-on), False (force-off)
dxvk.enableStarProfile = Auto

# FSR 1.0 upscaler: Auto, True, False
vegas.enableUpscaler = Auto
```

All other parameters (thresholds, tier, bind skip, HAAE, quality scaling) are auto-tuned by the Vegas engine.

---

## 🧱 Build from Source

```bash
git clone --recursive https://github.com/isygold/Star-Engine-DXVK-Releases.git
cd Star-Engine-DXVK-Releases

# Android cross-build (requires NDK + Meson)
meson setup --cross-file build-android-aarch64.txt \
  --buildtype release --prefix /output/dir build
cd build
ninja install
```

See `DXVK 2.7.2.1/dxvk/README.md` for upstream build instructions.

---

## 📜 Changelog (Vegas)

| Commit | Feature |
|--------|---------|
| `c7be5c3` | Master switch + dead code removal + conf docs |
| `c006dc3` | Framegen first-frame dead code fix |
| `7da716a` | 3-pass framegen integration |
| `42bcbe7` | FSR intermediate target + barrier validation |
| (earlier) | FSR SPIR-V + compute pipeline + HAAE base |

---

## 📝 Notes

- **Tier 1 (Adreno 610/619):** Frame generation disabled — compute budget insufficient for 3-pass. FSR available but not recommended.
- **BCn→ASTC transcoder:** Implemented but gated — will be enabled when image upload pipeline is wired.
- **Turnip driver:** Use a recent Turnip (Mesa 25.x+) for best results. The Vulkan 1.3 path is required for descriptor indexing and push constants.
- **Container tests:** Built-in benchmarks may show lower FPS due to draw thresholds. Judge by actual gameplay smoothness.

---

## 📜 Credits

- **Lead Developer:** ISYGOLD
- **Base Project:** DXVK v2.7.1+ by doitsujin
- **License:** zlib/libpng
