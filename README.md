<p align="center">
  <img alt="VEGAS" src="./vegas_banner.gif" width="80%">
  <br>
  <em>star engine rebased and rebranded; vegas</em>
</p>

# VEGAS — DXVK 2.4.1
### Stability Backport: GPLAsync + Performance Optimizations for Adreno Mobile

VEGAS is a specialized performance fork of **DXVK v2.4.1** (via the GPLAsync backport) targeting **Qualcomm Adreno GPUs** on Android emulation (Star Emulator / Winlator). It features automatic async shader compilation, tier-based auto-tuning, FSR 1.0 compute upscaling, motion-compensated frame generation, and a TBDR-aware dynamic governor — all configurable through simple DXVK options.

**This is the stable backport branch (`build-fix-2.4.1`).** It ships with auto-generated crash reports, per-game config presets, and automatic session tracking. A `dxvk.conf` file is bundled but **not required** — VEGAS works out of the box with sensible defaults. Only create one if you want to tweak specific behavior.

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
| 1 | 506-620 (low-end) | 100 | 30 | 1.5x |
| 2 | 630-690 (mid) | 200 | 50 | 1.8x |
| 3 | 7xx/8xx (high-end) | 350 | 100 | 2.5x |

Thresholds are Ph42oN GPLAsync battle-tested defaults. Unity engine games automatically receive higher headroom (`{200,400,700}`) via the game preset system.

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

### Per-Game Config Presets
Auto-detected from the game executable name — no config file needed:
- **Unity engine:** threshold `{200, 400, 700}` — accommodates higher draw batch volume
- **General:** threshold `{100, 200, 350}` — Ph42oN defaults for everything else

Override with `vegas.gameConfig = Unity` / `vegas.gameConfig = General` in `dxvk.conf`.

### Auto Crash Reports & Session Tracking
Every game session generates three files in the game directory automatically:
- `vegas-<game>.marker.txt` — written at session start, deleted on clean exit
- `vegas-<game>.report.json` — machine-readable report with FPS histogram, device info, and crash flag
- `vegas-<game>.issue.md` — pre-formatted GitHub issue body, ready to paste

Crash detection: if the game crashes or is force-killed, the orphaned marker file is detected on next launch and `crashed: true` is set in the new report. No manual placement or config required.

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

# Async shader compilation: True (default), False
dxvk.enableAsync = True

# GPL async state cache: False (default), True
dxvk.gplAsyncCache = False

# Per-game config preset: Auto (default), Unity, General
vegas.gameConfig = Auto

# Manual tier override (advanced): 0=auto, 1=low-end, 2=mid, 3=high-end
vegas.forceTier = 0

# Compiler thread count (advanced): 0=auto (max 4 on ARM64)
dxvk.numCompilerThreads = 0

# Draw profiling: VEGAS_PROFILE_DRAWS=1 enables per-frame CSV dump to /sdcard/
# (off by default — zero production impact)

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
- **Draw thresholds `{100,200,350}` are Ph42oN GPLAsync battle-tested defaults.** Unity games automatically receive `{200,400,700}` via the game preset system. These values do NOT affect rendering correctness — every draw call still renders, just with more efficient batching.

---

## FAQ

**Q: What is VEGAS?**
A: VEGAS is a performance fork of DXVK 2.4.1 with GPLAsync async pipeline compilation, specifically tuned for Qualcomm Adreno GPUs on Android emulation (Star Emulator / Winlator). It adds a tier-based auto-tuning engine, FSR 1.0 compute upscaling, motion-compensated frame generation, a TBDR-aware adaptive draw governor, per-game config presets, and automatic crash reports — all behind a single master switch.

**Q: How is VEGAS different from stock DXVK?**
A: Stock DXVK is designed for desktop GPUs with an immediate-mode renderer. Adreno GPUs are Tile-Based Deferred Renderers (TBDR) — accumulating too many draws before flushing the tile buffer causes catastrophic performance collapse. VEGAS addresses this with a TBDR-inverted governor (lowers draw threshold when CPU-bound, opposite of desktop DXVK), adaptive draw flushing, GPU pacing (HAAE), a tier system that auto-tunes thresholds per GPU capability, and bind-skip optimization.

**Q: Which GPUs are supported?**
A: VEGAS targets Qualcomm Adreno GPUs running Turnip Vulkan driver (Mesa 25.x+). Classification: Tier 1 (506-620 — entry), Tier 2 (630-690 — mid), Tier 3 (7xx/8xx — high-end). Non-Adreno GPUs (Mali, PowerVR) work in stock DXVK mode but VEGAS features are disabled unless `dxvk.enableStarProfile = True` is forced.

**Q: Where does VEGAS read its config from?**
A: VEGAS reads `dxvk.conf` from: `DXVK_CONFIG_FILE` environment variable, `/storage/emulated/0/Winlator/`, `/storage/emulated/0/Download/`, or `/storage/emulated/0/`. A default `dxvk.conf` is bundled in the WCP but is **not required** — VEGAS works out of the box. Only create one if you want to override specific behavior.

**Q: What's the difference between the DXVK-type and VEGAS-type WCP packages?**
A: Each release provides two WCP packages with **identical DLLs** — only the metadata in `profile.json` differs. The `dxvk-2.4.1-vegas-*.wcp` package has type `"DXVK"` for stock Winlator and general Android DXVK use. The `vegas-2.4.1-*.wcp` package has type `"VEGAS"` for Star Emulator's custom WCP installer. Use the DXVK-type package for Winlator and the VEGAS-type package for Star Emulator — the DLLs are the same either way.

**Q: What does the master switch do?**
A: `dxvk.enableStarProfile` (Auto / True / False): **Auto** (default) enables all VEGAS features on Adreno, disables on non-Adreno. **True** force-enables regardless of GPU. **False** hard-disables every VEGAS feature — the DLL behaves like stock DXVK. Use False as an emergency escape for problematic games.

**Q: The game crashes or doesn't launch — what should I try?**
A: Try in order: (1) `dxvk.enableStarProfile = False` — if the game launches, it's a VEGAS-specific issue. (2) `vegas.forceTier = 1` — conservative thresholds, frame gen disabled. (3) `dxvk.numCompilerThreads = 2` — reduce CPU contention. (4) Check logcat with `DXVK_LOG_LEVEL=debug` for error lines. If the game still crashes with the master switch off, the issue is DXVK/driver compatibility, not VEGAS.

**Q: How do I report a bug?**
A: Every game session auto-generates `vegas-<game>.report.json` and `vegas-<game>.issue.md` in the game directory. The `.issue.md` is a pre-formatted GitHub issue with device info, FPS histogram, and config snapshot — ready to paste at https://github.com/isygold/vegas-releases/issues. For manual reports, include: Adreno model, driver version, game title, your `dxvk.conf`, full debug logcat output, and reproduction steps.

---

## Credits

- **Lead Developer:** isygold
- **Base Project:** DXVK v2.4.1 by doitsujin
- **GPLAsync Patch:** Ph42oN (dxvk-gplasync v2.4-1), ishitatsuyuki (upstream GPLAsync)
- **FSR 1.0:** AMD GPUOpen (EASU compute shader)
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
