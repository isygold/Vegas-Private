<p align="center">
  <img alt="VEGAS" src="./vegas_banner.gif" width="80%">
  <br>
  <em>star engine rebased and rebranded; vegas</em>
</p>

# VEGAS — DXVK 2.4.1

[![Downloads](https://img.shields.io/github/downloads/isygold/vegas-releases/total?color=blue&style=flat-square)](https://github.com/isygold/vegas-releases/releases)
[![Stars](https://img.shields.io/github/stars/isygold/vegas-releases?style=flat-square)](https://github.com/isygold/vegas-releases)
[![Latest Release](https://img.shields.io/github/v/release/isygold/vegas-releases?style=flat-square)](https://github.com/isygold/vegas-releases/releases/latest)
[![Sponsor](https://img.shields.io/badge/Sponsor-%24?logo=github&style=flat-square)](https://github.com/sponsors/isygold/card)

📖 **[Wiki (user guide)](https://github.com/isygold/vegas-releases/wiki)** · **[Wiki (dev guide)](https://github.com/isygold/Vegas-Private/wiki)**

### Stability Backport: GPLAsync + Performance Optimizations for Adreno Mobile

VEGAS is a specialized performance fork of **DXVK v2.4.1** (via the GPLAsync backport) targeting **Qualcomm Adreno GPUs** on Android emulation (Star Emulator / Winlator & its forks/GAMEHUB/GAMENATIVE/BANNERLATOR). It features automatic async shader compilation, tier-based auto-tuning, FSR 1.0 compute upscaling, motion-compensated frame generation, and a TBDR-aware dynamic governor — all configurable through simple DXVK options.

**This is the stable release line (`release-v2.4.1`, tag `v2.4.1-V`).** It ships with auto-generated crash reports, per-game config presets, and automatic session tracking. A `dxvk.conf` file is bundled but **not required** — VEGAS works out of the box with sensible defaults. Only create one if you want to tweak specific behavior.

---

## ⚠️ Disclaimer

The frame generation (FG) and FSR features in VEGAS are provided
**for testing and evaluation purposes only**. Enabling frame
generation alongside other interpolation or upscaling features
(including the emulator's own FSR, LSFG-VK, or bionic framegen)
may produce compounding artifacts, increased latency, or unexpected
behavior. The authors of VEGAS assume **no responsibility for any
loss, damage, game crashes, or degraded experience** that may result
from using these features. By using VEGAS you accept full
responsibility for testing and evaluating it in your own environment.

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

| Tier | Adreno GPUs | Bake Threshold | HAAE Threshold |
|------|-------------|----------------|----------------|
| 1 | 5xx, 6xx < 620 (entry) | 100 | 30 |
| 2 | 6xx 620-689, 7xx < 730 (mid) | 200 | 50 |
| 3 | 690+, 7xx >= 730, 8xx+ (high-end) | 350 | 100 |

Bake thresholds seed the governor — the **runtime** threshold is computed by the
governor every frame (see below). Unity engine games automatically receive
`{200, 400, 700}` via the per-game preset system.

### FSR 1.0 Compute Upscaler
Full FSR 1.0 EASU compute pipeline with async dispatch via timeline semaphore:
- Upscales when render resolution is meaningfully below swapchain resolution (auto mode, ratio < 0.85)
- Eliminates 0.5-1.0ms CPU stall on every upscaled frame

### 3-Pass Motion-Compensated Frame Generation
Available on Tier 2 (<= 29ms frametime) and Tier 3 (<= 33ms frametime):
1. Motion estimation — block SAD on prev/cur frames
2. Median filter — 3x3 spatial denoise on motion field
3. Warp + blend — warp prev frame by filtered motion, alpha-blend at 0.5

Compute-only pipeline. Disabled on Tier 1 (insufficient compute budget).

Frame generation (sometimes called "motion interpolation" or "fake frames") works by analysing the motion between two consecutively rendered frames and synthesising an in-between frame. The result is smoother motion at the same rendering cost — the GPU still renders at the native framerate, but the display shows twice as many frames. Think of it as the mobile equivalent of DLSS 3 Frame Generation.

Toggle with `vegas.enableFramegen`:
- **Auto** (default): enabled on Tier 2/3 when headroom allows
- **True**: force-enable regardless of tier (useful for testing)
- **False**: disable entirely, even on capable hardware

### FSR Upscaler (FidelityFX Super Resolution 1.0 EASU)
Spatial upscaler that sharpens and rescales the back buffer to the
presentation surface. Runs **before** framegen in the present path:
FSR upscales (spatial), then framegen interpolates (temporal) — the
smoother-motion benefit stacks on the sharper image.

Toggle with `vegas.enableUpscaler`:
- **Auto** (default): upscale only when the back buffer is meaningfully
  smaller than the presentation surface (games rendering at internal
  resolutions below the screen)
- **True**: force upscale regardless of size (testing only)
- **False**: disable entirely

Note: FSR adds latency from the compute pass. Combined FSR + framegen is
supported, but treat both-on as experimental — see the Disclaimer above.

### Adaptive Governor — TBDR-Aware Submission Pacing
Three-function pipeline running each frame (`updateFrameTiming` → `calculateThreshold` → `endOfFrameCleanup`):
- **Draw-load density metric** (draws/ms, EMA-smoothed) — the reliable geometry signal under Wine where the classic GPU-load sensor is broken
- **Closed-loop `targetFlushes` tuning** (1–32/frame): real GPU load from device `GpuIdleTicks` drives flush count — lagging + low GPU load → CPU-bound → reduce flushes; lagging + high GPU load → GPU-bound → raise flushes for finer TBDR batching
- **Proportional predictor** — threshold = previous frame draws / targetFlushes, with a self-calibrating `dynamicMaxBatchCap` (rolling-max based)
- **Atomic-split dual-mode** — small scenes split at `floorMinimumCap` (600, TR13-validated tile-overflow safety line); heavy 3D frames capped by the batch cap
- **Variance guard** on the 120-frame rolling window — disabled after it poisoned the threshold (single pause frame broke tuning for 120+ frames)

**Why TBDR-aware?** Desktop DXVK batches aggressively on CPU-bound frames. On tile-based Adreno, unbounded batching overflows the tile buffer; the governor instead splits submissions to match the tile budget, and only batches when the draw profile says it is safe.

### Per-Game Config Presets
Auto-detected from the game executable name — no config file needed:
- **Unity engine:** bake thresholds `{200, 400, 700}`, HAAE `{50, 80, 150}` — accommodates higher draw batch volume
- **General:** bake thresholds `{100, 200, 350}`, HAAE `{30, 50, 100}` — Ph42oN defaults for everything else

### Auto Crash Reports & Session Tracking
Every game session generates three files in the game directory automatically:
- `vegas-<game>.marker.txt` — written at session start, deleted on clean exit
- `vegas-<game>.report.json` — machine-readable report with FPS histogram, device info, and crash flag
- `vegas-<game>.issue.md` — pre-formatted GitHub issue body, ready to paste

Crash detection: if the game crashes or is force-killed, the orphaned marker file is detected on next launch and `crashed: true` is set in the new report. No manual placement or config required.

Clean exit: the marker is removed on normal process exit (Wine DLL-detach hook) and on orderly device teardown — quitting a game normally never leaves a stale marker, so it won't be misreported as a crash. A force-stop from the launcher still counts as an unclean exit (by definition) and leaves the marker.

Draw-count profiling is separate: `vegas.profileDraws = true` in dxvk.conf (or the equivalent `VEGAS_PROFILE_DRAWS=1` env var — either works) enables the per-frame CSV dump to `/sdcard/vegas_<game>_drawcount.csv` (append mode, accumulates across sessions). Unset, nothing is recorded and the file is never touched. `vegas.telemetry = true` enables frame-generation fill/bind/readback stats in logcat (diagnostics only).

### VegasHud Performance Overlay
- Lightweight performance HUD with frame-skip optimization (updates every 5th frame)
- Draw call count, frame time, GPU load, tier info
- Configurable via standard `DXVK_HUD` environment variable
- Legacy `cpu` token (per-core CPU load) was removed in this build

### Dynamic VRAM & GPU Mask
- VRAM clamped to ~40% of system RAM (1-4 GB range)
- Adreno tier mapped to compatible NVIDIA vendor/device ID for game compatibility

---

## Installation

### WCP Package Types
Each release provides **two** WCP packages with identical DLLs but different metadata:

| Package | Type field | For |
|---------|-----------|-----|
| `dxvk-2.4.1-vegas-*.wcp` | DXVK | Stock Winlator & its forks/GAMEHUB/GAMENATIVE and general Android DXVK use |
| `vegas-2.4.1-*.wcp` | VEGAS | Star Emulator/Bannerlator (latest build) |

> **Note — can't find a config file for a build?**
> All VEGAS builds and their artifacts (including the `vegas-config-*`
> `dxvk.conf`) are stationed on the development repo **before** they become
> releases. If what you need isn't attached to a release at
> **github.com/isygold/vegas-releases**, go to
> **github.com/isygold/Vegas-Private** — that's where every build and artifact
> lives until it's promoted to a release (Actions tab → artifact list).

### Via Star Emulator/Bannerlator
1. Open Star Emulator/Bannerlator emulator
2. Go to **Contents** menu
3. Install the `vegas-2.4.1-*.wcp` package (VEGAS-native type)

### Via Stock Winlator & its forks/GAMEHUB/GAMENATIVE
1. Download the `dxvk-2.4.1-vegas-*.wcp` package
2. Install it as a standard DXVK WCP package in Winlator

### Manual Configuration (optional)
A `dxvk.conf` is bundled in the WCP but **not required** — VEGAS works out of the box. Only place a custom `dxvk.conf` if you want to override defaults:

- `/storage/emulated/0/Winlator/`
- `/storage/emulated/0/Download/`
- `/storage/emulated/0/`

Or set `DXVK_CONFIG_FILE` to your config path.

---

## Configuration

```ini
# Master switch: Auto (Adreno only), True (force-on), False (force-off)
dxvk.enableStarProfile = Auto

# Frame generation: Auto (tier-gated), True (force), False (disable)
vegas.enableFramegen = Auto

# FSR upscaler: Auto (back buffer < 85% of surface), True (force), False (disable)
vegas.enableUpscaler = Auto

# Async shader compilation: True (default), False
dxvk.enableAsync = True

# GPL async state cache: False (default), True
dxvk.gplAsyncCache = False

# Per-game config preset: auto-detected from exe name (no option — Unity
# gets {200,400,700}, everything else {100,200,350})

# Manual tier override (advanced): 0=auto, 1=entry, 2=mid, 3=high-end
vegas.forceTier = 0

# Compiler thread count (advanced): 0=auto (hardware concurrency, max 64)
dxvk.numCompilerThreads = 0

# Draw profiling: vegas.profileDraws = true enables per-frame CSV dump to
# /sdcard/ (off by default — zero production impact)
# vegas.profileDraws = true

# FG telemetry: vegas.telemetry = true logs fill/bind/readback stats to logcat
# vegas.telemetry = true

# Environment variable overrides:
# DXVK_ASYNC=0         → disable async compilation
# DXVK_GPLASYNCCACHE=1 → enable GPL state cache
```

All other parameters (draw thresholds, bind skip, HAAE pacing, quality scaling) are auto-tuned by the VEGAS engine.

---

## Notes

- **This is a backport, not a VEGAS upgrade path.** VEGAS 2.4.1 is a backport of GPLAsync + VEGAS performance features onto DXVK 2.4.1. It is NOT an upgrade from an earlier VEGAS version — it is a separate stable branch. The feature branch (`2.7.4-beta`) with the full DXVK v2.7.3 base and GPU transcoder is a different track.
- **Faster than stock DXVK and plain GPLAsync.** The combination of async shader compilation (GPLAsync), TBDR-aware governor, FSR upscaling, and low-latency draw thresholds makes VEGAS 2.4.1 faster and smoother than both stock DXVK 2.4.1 and standalone dxvk-gplasync builds. Users upgrading from either will see measurable improvements.
- **No config file required.** A `dxvk.conf` is bundled in the WCP for reference, but VEGAS works immediately with sensible defaults. Only create a custom `dxvk.conf` if you want to tweak specific behavior.
- **Tier 1 (Adreno 5xx/6xx low-end):** Frame generation disabled. FSR available but not recommended at very low resolutions. Zero-init enabled for Turnip stability.
- **Turnip driver:** Use Mesa 25.x+ with Vulkan 1.3 support for best results.
- **Synthetic benchmarks:** May show lower FPS than stock due to draw thresholds. Judge performance by actual gameplay smoothness.
- **GPU-bound workloads:** VSync-off provides negligible gain when the GPU is already saturated (17+ ms frame times).
- **Bake thresholds `{100,200,350}` are Ph42oN GPLAsync battle-tested defaults.** Unity games automatically receive `{200,400,700}` via the game preset system. These are the governor's seed values — the runtime threshold is recomputed every frame from the draw-load predictor and never drops below `floorMinimumCap` (600). Thresholds do NOT affect rendering correctness — every draw call still renders, just with more efficient batching.
- **Governor v4.2.1d (final).** The split logic counts every draw type (including indirect draws), measures draws since the last submission rather than per-frame totals, and enforces a 600-draw per-render-pass safety floor. Bind-skip is invalidated at every command-buffer boundary so a mid-frame flush can never leave geometry unbound. Verified smooth on Tomb Raider (2013) and Hollow Knight on Adreno 610 — zero tile-overflow artifacts, zero governor flushes at steady state.

---

## FAQ

**Q: What is VEGAS?**
A: VEGAS is a performance fork of DXVK 2.4.1 with GPLAsync async pipeline compilation, specifically tuned for Qualcomm Adreno GPUs on Android emulation (Star Emulator / Winlator). It adds a tier-based auto-tuning engine, FSR 1.0 compute upscaling, motion-compensated frame generation, a TBDR-aware adaptive draw governor, per-game config presets, and automatic crash reports — all behind a single master switch.

**Q: How is VEGAS different from stock DXVK?**
A: Stock DXVK is designed for desktop GPUs with an immediate-mode renderer. Adreno GPUs are Tile-Based Deferred Renderers (TBDR) — accumulating too many draws before flushing the tile buffer causes catastrophic performance collapse. VEGAS addresses this with a TBDR-aware governor (draw-load density metric instead of the broken Wine GPU-load sensor, closed-loop submission pacing from real GpuIdleTicks, atomic-split dual-mode with a 600-draw tile-overflow safety floor), a tier system that auto-tunes bake thresholds per GPU capability, and bind-skip optimization.

**Q: Which GPUs are supported?**
A: VEGAS targets Qualcomm Adreno GPUs running Turnip Vulkan driver (Mesa 25.x+). Classification: Tier 1 (5xx, 6xx < 620 — entry), Tier 2 (6xx 620-689, 7xx < 730 — mid), Tier 3 (690+, 7xx >= 730, 8xx+ — high-end). Non-Adreno GPUs (Mali, PowerVR) work in stock DXVK mode but VEGAS features are disabled unless `dxvk.enableStarProfile = True` is forced.

**Q: Where does VEGAS read its config from?**
A: VEGAS reads `dxvk.conf` from: `DXVK_CONFIG_FILE` environment variable, `/storage/emulated/0/Winlator/`, `/storage/emulated/0/Download/`, or `/storage/emulated/0/`. A default `dxvk.conf` is bundled in the WCP but is **not required** — VEGAS works out of the box. Only create one if you want to override specific behavior.

**Q: What's the difference between the DXVK-type and VEGAS-type WCP packages?**
A: Each release provides two WCP packages with **identical DLLs** — only the metadata in `profile.json` differs. The `dxvk-2.4.1-vegas-*.wcp` package has type `"DXVK"` for stock Winlator & its forks /GAMEHUB/GAMENATIVE and general Android DXVK use. The `vegas-2.4.1-*.wcp` package has type `"VEGAS"` for Star Emulator's custom WCP installer. Use the DXVK-type package for Winlator and the VEGAS-type package for Star Emulator/Bannerlator — the DLLs are the same either way.

**Q: What does the master switch do?**
A: `dxvk.enableStarProfile` (Auto / True / False): **Auto** (default) enables all VEGAS features on Adreno, disables on non-Adreno. **True** force-enables regardless of GPU. **False** hard-disables every VEGAS feature — the DLL behaves like stock DXVK. Use False as an emergency escape for problematic games.

**Q: The game crashes or doesn't launch — what should I try?**
A: Try in order: (1) `dxvk.enableStarProfile = False` — if the game launches, it's a VEGAS-specific issue. (2) `vegas.forceTier = 1` — conservative thresholds, frame gen disabled. (3) `dxvk.numCompilerThreads = 2` — reduce CPU contention. (4) Check logcat with `DXVK_LOG_LEVEL=debug` for error lines. If the game still crashes with the master switch off, the issue is DXVK/driver compatibility, not VEGAS.

**Q: Does frame generation increase FPS?**
A: No. Framegen makes the display show more frames per second by
synthesising in-between frames, but the GPU still renders at the
native framerate. The FPS counter shows 60fps because the display
is receiving 60 frames — 30 real, 30 interpolated. The GPU didn't
get faster; it's doing the same work. Framegen is a smoothness
feature, not a performance feature. For real FPS gains, use FSR
upscaling or the TBDR-aware governor.

**Q: How do I report a bug?**
A: Every game session auto-generates `vegas-<game>.report.json` and `vegas-<game>.issue.md` in the game directory. The `.issue.md` is a pre-formatted GitHub issue with device info, FPS histogram, and config snapshot — ready to paste at https://github.com/isygold/vegas-releases/issues. For manual reports, include: Adreno model, driver version, game title, your `dxvk.conf`, full debug logcat output, and reproduction steps.

---

## Credits

- **Lead Developer:** isygold
- **Base Project:** DXVK v2.4.1 by doitsujin
- **GPLAsync Patch:** Ph42oN (dxvk-gplasync v2.4-1), ishitatsuyuki (upstream GPLAsync)
- **FSR 1.0:** AMD GPUOpen (EASU compute shader)
- **Testing & Feedback:** @H0tIce77 — consistent Adreno device testing and detailed logs since Star Engine DXVK 2.7.2.1
- **Framegen Testing:** @devaspe
- **License:** zlib/libpng

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
- `DXVK_CONFIG_FILE` — Point to a custom config path

### Device Filter
`DXVK_FILTER_DEVICE_NAME="Adreno"` to select a specific Vulkan device if multiple GPUs are present.

### Anti-Cheat Warning
Modifying Direct3D libraries in multiplayer games may result in account bans. **Use at your own risk.**
