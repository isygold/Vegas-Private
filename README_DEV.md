# VEGAS Developer Guide — DXVK 2.4.1 Backport

Welcome to the VEGAS (formerly Star Engine) developer documentation for the
**stability backport branch (`build-fix-2.4.1`)**. This branch combines:
- DXVK v2.4.1 base (proven stable with Hollow Knight)
- Ph42oN's GPLAsync v2.4-1 patch (async shader compilation)
- VEGAS performance features: FSR 1.0, framegen, TBDR governor, VegaHud

Unlike the feature branch (`2.7.4-beta`), this branch does NOT include the GPU
BCn-to-ASTC transcoder or leegao's DxvkFence timeline semaphore.

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [Source Map](#2-source-map)
3. [Feature Reference](#3-feature-reference)
   - 3.1  GPLAsync — Async Pipeline Compilation
   - 3.2  Tier Classification
   - 3.3  Adaptive Governor (TBDR-Inverted)
   - 3.4  GPU Pacing (HAAE)
   - 3.5  Pipeline Bind-Skip
   - 3.6  Shader Zero-Init
   - 3.7  FSR 1.0 Upscaler
   - 3.8  Frame Generation (3-Pass)
   - 3.9  VegaHud Performance Overlay
   - 3.10  GPU Persona & VRAM Masking
   - 3.11  Performance Analysis & Logging
4. [Config Options Reference](#4-config-options)
5. [Adding a New Feature](#5-adding-a-new-feature)
6. [Testing Methodology](#6-testing-methodology)
7. [Common Pitfalls](#7-common-pitfalls)
8. [CI Pipeline & WCP Dual Build](#8-ci-pipeline--wcp-dual-build)
9. [Credits & Contributors](#9-credits--contributors)

---

## 1. Architecture Overview

VEGAS extends DXVK with ~60 static functions and ~45 static variables that
modify Vulkan command buffer dispatch, swapchain behavior, shader compilation,
GPU pacing, and HUD rendering — all gated behind a configurable feature set.

### Integration Points

```
 Game (D3D11/D3D9)
     |
     v
 +------------------------------+
 |  dxvk_context.cpp            |  draw()/drawIndexed() flush, bindSkip,
 |                              |  GPLAsync compat check
 +------------------------------+
 |  dxvk_device.cpp             |  HAAE submission throttle, VEGAS signature
 +------------------------------+
 |  dxvk_graphics.cpp           |  GPLAsync: async pipeline compilation path
 |  dxvk_graphics.h             |  fast-link fallback, async worker
 +------------------------------+
 |  dxvk_image.h                |  RT binding frame tracking (GPLAsync)
 +------------------------------+
 |  dxvk_vegas.cpp              |  ALL VEGAS feature logic, decision helpers,
 |  dxvk_vegas.h                |  FSR, framegen, VegaHud, governor
 +------------------------------+
 |  hud/dxvk_hud_item.cpp       |  HUD version display ("VEGAS" branding)
 +------------------------------+
 |  dxvk_options.h/.cpp         |  Config: enableAsync, gplAsyncCache,
 |                              |  enableStarProfile, vegasForceTier
 +------------------------------+
 |  star_fsr_spv.h              |  FSR 1.0 EASU SPIR-V
 |  star_fg_spv.h               |  Framegen 3-pass SPIR-V
 +------------------------------+
```

### Data Flow

```
InitializeProfile(DxvkDevice*)
  -> detect Adreno, classify tier, bake thresholds
  -> store VkDevice/VkQueue for FSR/FG
  -> called once from dxvk_vegas.cpp configure()

Per-Frame (PresentBase):
  measure frameTime
  -> compute GPU load from frameTime/target ratio
  -> analyzePerformance() -> tuneThreshold() -> TBDR-inverted governor
  -> shouldUpscale() -> FSR dispatch
  -> needsFrameGen() -> framegenDispatch() if eligible
  -> pushMetrics() -> stores gpuLoad, frameTime, perfState in Vegas statics

Per-Draw (draw/drawIndexed):
  shouldFlush(drawCount) -> spill render pass if over threshold
  shouldSkipBind() -> skip vkCmdBindPipeline if same handle
  checkAsyncCompilationCompat() -> async mode for RT-bound shaders

Per-Submit (submitCommandList):
  shouldSubmitHaae() -> inject empty fence submit for GPU pacing
```

---

## 2. Source Map

### Core Vegas Module

| File | Purpose | Key Contents |
|------|---------|--------------|
| `src/dxvk/dxvk_vegas.h` | All declarations | `Vegas` class (60+ static methods), `VegasProfile` struct, `VegasPerformanceState` enum, 45+ static member variables |
| `src/dxvk/dxvk_vegas.cpp` | All implementations | ~3100 lines. Tier classifier, TBDR-inverted governor, FSR dispatch, FG dispatch, VegaHud, pushMetrics() + getters, static variable definitions |

### GPLAsync Integration Points

| File | Purpose |
|------|---------|
| `src/dxvk/dxvk_graphics.cpp` | `getPipelineHandle(..., bool async)` — fast-link fallback when async |
| `src/dxvk/dxvk_graphics.h` | `m_asyncMutex`, `m_async` flag, `gplAsyncCache` |
| `src/dxvk/dxvk_context.cpp` | `checkAsyncCompilationCompat()` — RT frame tracking |
| `src/dxvk/dxvk_image.h` | `m_rtBindingFrameId` / `m_rtBindingFrameCount` — 5-frame minimum |
| `src/dxvk/dxvk_options.cpp` | enableAsync (default true), gplAsyncCache (default false) |
| `src/dxvk/dxvk_options.h` | Option declarations |
| `meson.build` | vcs_tag with `--dirty=-1-vegas` |

### DXVK Integration Points

| File | Lines (approx) | What Vegas Does There |
|------|----------------|-----------------------|
| `src/dxvk/dxvk_context.cpp` | 83-103, 839-862, 949-973 | Draw flush, bindSkip, GPLAsync compat |
| `src/dxvk/dxvk_context.h` | 830-834 | Vegas profile members |
| `src/dxvk/dxvk_device.cpp` | 260-264 | VEGAS signature comment |
| `src/dxvk/hud/dxvk_hud_item.cpp` | 86-98 | HUD shows "VEGAS" version string |

### DXGI Integration Points

| File | Lines (approx) | What Vegas Does There |
|------|----------------|-----------------------|
| `src/dxgi/dxgi_swapchain.cpp` | 348-503 | Frame timing, governor, FSR, framegen, pushMetrics() |

### SPIR-V Shaders

| File | Purpose |
|------|---------|
| `src/dxvk/star_fsr_spv.h` | FSR 1.0 EASU compute shader |
| `src/dxvk/star_fg_spv.h` | 3-pass framegen shaders (motion, median, warp) |

---

## 3. Feature Reference

### 3.1 GPLAsync — Async Pipeline Compilation

**Files:** `src/dxvk/dxvk_graphics.cpp`, `src/dxvk/dxvk_graphics.h`, `src/dxvk/dxvk_options.cpp`

This is a backport of Ph42oN's `dxvk-gplasync` patch for DXVK 2.4
(commit `4e5658e97b6c`). It enables asynchronous shader compilation via a
worker thread, eliminating the stutter that occurs when a game encounters
a new shader for the first time.

**Core mechanism:**
```
getPipelineHandle(state, async):
  if instance not found AND async:
    -> return empty handle immediately (no wait)
    -> pipeline will compile in background worker thread
    -> fast-link fallback renders the frame with a base pipeline

  if instance not found AND NOT async:
    -> block until pipeline is compiled (standard DXVK path)
```

**RT frame tracking:**
To prevent missing geometry from shaders that haven't finished compiling,
GPLAsync requires 5+ consecutive frames with the same render-target binding
before enabling async mode. This ensures critical shaders (depth-pass,
shadow-map) are compiled synchronously.

**Config:**
- `dxvk.enableAsync = true` (default) — async compilation ON
- `dxvk.gplAsyncCache = false` (default) — state cache OFF (uses fast-linking)
- `DXVK_ASYNC=0` — env var to disable
- `DXVK_GPLASYNCCACHE=1` — env var to enable state cache

---

### 3.2 Tier Classification

**File:** `src/dxvk/dxvk_vegas.cpp`

| Function | Lines | Purpose |
|----------|-------|---------|
| `classifyAdrenoTier(const char* deviceName)` | 89-122 | Parses device name into tier 1/2/3 |
| `Vegas::configure()` | 450-560 | Master init: classify tier, bake thresholds |
| `Vegas::getTier()` | ~560 | Returns `s_tier` |

**Tier Mapping:**

| Condition | Tier | Draw Threshold | HAAE Threshold | Cap Multiplier |
|-----------|------|----------------|----------------|----------------|
| gen <= 5, or 6xx < 620 | 1 (entry) | 50 | 30 | 1.5x |
| 6xx 620-689, or 7xx < 730 | 2 (mid) | 150 | 50 | 1.8x |
| 690+, 7xx >= 730, or 8xx+ | 3 (high) | 300 | 100 | 2.5x |

**Config override:** `vegas.forceTier = 0` (auto), `1`/`2`/`3` (manual)

**D3D9 override:** Higher thresholds for D3D9 games: `{300, 500, 800}`.

---

### 3.3 Adaptive Governor (TBDR-Inverted)

**File:** `src/dxvk/dxvk_vegas.cpp`

| Function | Lines | Purpose |
|----------|-------|---------|
| `tuneThreshold(uint32_t&, float, float, uint32_t)` | 210-249 | TBDR-inverted governor |
| `tuneThreshold(float, float)` | 253-275 | Self-contained: EMA + adaptive cooldown |

**Governor Logic (TBDR-inverted):**
```
if (load > 0.85f AND frameTime > 20.0f)   -> cap (GPU-bound, RAISE threshold)
if (load < 0.45f AND frameTime > 10.0f)    -> floor (CPU-bound, LOWER threshold)
else                                        -> base (balanced, reset)
```

**Cap Multipliers (C2 re-tune):**
- Tier 1: 1.5x (50 -> 75 max)
- Tier 2: 1.8x (150 -> 270 max)
- Tier 3: 2.5x (300 -> 750 max)

**Floor (CPU-bound flush):** `max(50, base / 2)`

**EMA Smoothing:** `s_smoothFt = s_smoothFt * 0.9 + frameTime * 0.1`

**Adaptive Cooldown:** `ceil(ft * 0.3)`, clamped [5, 30] frames
- At 60fps (16.7ms): ~5 frame cooldown
- At 30fps (33.3ms): ~10 frame cooldown

---

### 3.4 GPU Pacing (HAAE)

**File:** `src/dxvk/dxvk_vegas.cpp`

| Function | Lines | Purpose |
|----------|-------|---------|
| `shouldSubmitHaae(uint32_t&, uint32_t)` | ~575 | Returns true when accumulated draws >= threshold |

**Threshold per tier:** `{30, 50, 100}` (Tier 1 gets most frequent pacing).

**Behavior:** When triggered, submits an empty `DxvkSubmitInfo` with a fence.
This acts as a GPU pacemaker, preventing the submission queue from growing
too large on TBDR architectures.

---

### 3.5 Pipeline Bind-Skip

**File:** `src/dxvk/dxvk_vegas.cpp`

| Function | Lines | Purpose |
|----------|-------|---------|
| `shouldSkipBind()` | ~570 | Returns `s_enabled && s_bindSkipEnabled` |

**What it skips:** `vkCmdBindPipeline` when the same pipeline handle was
already bound and pipeline state has not changed. Reduces CPU overhead on
the draw call path.

---

### 3.6 Shader Zero-Init

**File:** `src/dxvk/dxvk_vegas.cpp`

| Function | Lines | Purpose |
|----------|-------|---------|
| `shouldZeroInit(uint32_t tier)` | ~280 | Returns `tier < 3` |

**Behavior:**
- Tier 1/2 -> zero-init ON (safety against Turnip hangs from uninitialized workgroup memory)
- Tier 3 -> zero-init OFF (~1-2% shader performance gain)

---

### 3.7 FSR 1.0 Upscaler

**File:** `src/dxvk/dxvk_vegas.cpp`

| Function | Lines | Purpose |
|----------|-------|---------|
| `Vegas::fsrDispatch()` | ~1200-1800 | FSR 1.0 EASU compute dispatch |
| `Vegas::shouldUpscale()` | ~570 | Resolves Tristate + extent check |
| `initFsrPipeline(VkDevice)` | ~1000-1200 | Creates FSR compute pipeline |

**Called from:** `src/dxgi/dxgi_swapchain.cpp` (PresentBase)

**Behavior:**
- Upscales when swapchain extent > render extent
- Uses compute shader dispatch (EASU)
- Configurable via `dxvk.enableStarProfile` (master switch)

**FSR ratio guard (C1):** Skips FSR if source is already >= 85% of target
resolution in both dimensions. Prevents useless GPU dispatch when upscaling
would not be visually beneficial.

---

### 3.8 Frame Generation (3-Pass)

**File:** `src/dxvk/dxvk_vegas.cpp`

| Function | Lines | Purpose |
|----------|-------|---------|
| `Vegas::framegenDispatch()` | ~2000-3000 | Full 3-pass motion-compensated FG |
| `Vegas::needsFrameGen()` | ~330 | Tier-based eligibility |
| `initFgPipeline(VkDevice)` | ~1800-2000 | Creates 3 compute pipelines |

**3 Passes:**
1. Motion search (block SAD on prev/cur frames)
2. Median filter (3x3 spatial denoise on motion field)
3. Warp + blend (warp prev frame by filtered motion, alpha-blend at 0.5)

**Eligibility:**
- Tier 1: never (compute budget insufficient)
- Tier 2: frameTime <= 29ms
- Tier 3: frameTime <= 33ms

**Framegen timeout:** If GPU dispatch takes longer than 50ms, the frame is
skipped (calls `fgCleanup(8)` and returns false). Prevents present thread
deadlock on stalled GPU.

---

### 3.9 VegaHud Performance Overlay

**File:** `src/dxvk/dxvk_vegas.cpp`

| Function | Lines | Purpose |
|----------|-------|---------|
| `pushMetrics()` | ~3100 | Stores per-frame metrics (load, ft, state) |
| `getDrawThreshold()` | ~558 | Returns current draw threshold |
| `getDrawCount()` | ~560 | Returns accumulated draw count |

**Render path:** The HUD renders via the standard `DXVK_HUD` frametime graph.
The version string shows "VEGAS" branding instead of "DXVK".

**Frame-skip optimization (C3):** `pushMetrics()` writes data only every 5th
call via `thread_local s_hudSkip` counter. HUD updates at ~12fps instead of
~60fps — smooth enough for monitoring, zero impact on render path.

**Data flow:**
```
dxgi_swapchain.cpp:PresentBase
  -> Vegas::pushMetrics(gpuLoad, frameTime, perfState, ...)
    -> writes to Vegas static members (every 5th call)

hud/dxvk_hud_item.cpp
  -> reads Vegas static members for HUD display
  -> shows "VEGAS 2.4.1" version string from @VCS_TAG@
```

---

### 3.10 GPU Persona & VRAM Masking

**File:** `src/dxvk/dxvk_vegas.cpp`

| Function | Lines | Purpose |
|----------|-------|---------|
| `applyGpuMask(Config&)` | ~285-323 | Maps Adreno tier -> NVIDIA vendor/device ID |
| `applyVramSwap(Config&)` | ~272-282 | Sets VRAM to 40% of system RAM |

**GPU Persona Mapping:**

| Tier | NVIDIA Persona | Vendor ID | Device ID |
|------|---------------|-----------|-----------|
| 1 | GTX 1050 Ti | 10de | 1c82 |
| 2 | GTX 1070 | 10de | 1b81 |
| 3 | RTX 3060 | 10de | 2503 |

**VRAM:** Sets `dxgi.maxDeviceMemory` to 40% of total RAM (clamped 1-4 GB).

---

### 3.11 Performance Analysis & Logging

**File:** `src/dxvk/dxvk_vegas.cpp`

| Function | Lines | Purpose |
|----------|-------|---------|
| `analyzePerformance(float, float, float)` | ~343-363 | Classifies frame state |
| `getStatusString(VegasPerformanceState)` | ~376-384 | Maps state -> "NORMAL" etc. |

**Thresholds:**
- Normal: everything else
- Lagging: frameTime >= 1.5x target
- Stuttering: frame-to-frame delta > 1.25x target
- Overheating: load >= 0.95 AND frameTime >= 3.0x target

**GPU Load Estimate (ftRatio proxy):**
```
ftRatio = frameTime / targetFrameTime
> 2.0  -> 0.96 (overheating)
> 1.5  -> 0.92 (badly lagging)
> 1.2  -> 0.85 (saturated)
> 0.9  -> 0.65 (near capacity)
> 0.5  -> 0.40 (some headroom)
else   -> 0.25 (lots of headroom)
```

---

## 4. Config Options

| Config Key | Type | Default | Declaration | Purpose |
|---|---|---|---|---|
| `dxvk.enableAsync` | bool | true | `dxvk_options.h:29` | Async pipeline compilation |
| `dxvk.gplAsyncCache` | bool | false | `dxvk_options.h:31` | GPL state cache with fixes |
| `dxvk.enableStarProfile` | Tristate | Auto | `dxvk_options.h:52` | Master switch for VEGAS features |
| `vegas.forceTier` | int32 | 0 | `dxvk_options.h:55` | Override GPU tier detection |
| `dxvk.enableGraphicsPipelineLibrary` | Tristate | Auto | `dxvk_options.h:23` | Vulkan GPL support |
| `dxvk.numCompilerThreads` | int32 | 0 | `dxvk_options.h:20` | Override compiler thread count |

**Environment variable overrides:**
| Env Var | Effect |
|---------|--------|
| `DXVK_ASYNC=0` | Disable async compilation (overrides `dxvk.enableAsync`) |
| `DXVK_GPLASYNCCACHE=1` | Enable GPL state cache (overrides `dxvk.gplAsyncCache`) |
| `DXVK_HUD=...` | Standard DXVK HUD configuration |

---

## 5. Adding a New Feature

### Step-by-step workflow:

1. **Declare in `dxvk_vegas.h`:**
   - Add new static method to the `Vegas` class
   - Add any new static member variables (baked state)
   - Add any new structs/enums needed

2. **Implement in `dxvk_vegas.cpp`:**
   - Define static variables at the top (lines 31-75 area)
   - Implement the method
   - If it needs device information, integrate with `Vegas::configure()`
   - If it's a per-frame decision, expose a `shouldX()` or `xDispatch()` method

3. **Wire into the DXVK pipeline:**
   - Find the right integration point (draw, present, submit, create)
   - Add the Vegas call behind a guard:
     ```cpp
     if (unlikely(Vegas::shouldX(...))) { ... }
     ```
   - Use `unlikely()` macro for branches that are infrequently taken

4. **Add config option if needed:**
   - Declare in `dxvk_options.h`
   - Read in `dxvk_options.cpp`

5. **Add logging:**
   - Use `Logger::debug()` for diagnostic messages

6. **Test:**
   - Test on real Adreno hardware (6xx, 7xx if possible)
   - Test with `dxvk.enableStarProfile = False` (feature should be a no-op)

---

## 6. Testing Methodology

### Hardware Requirements
- Real Adreno 6xx device (preferably 610/619 for Tier 1, 640+ for Tier 2)
- Turnip driver (Mesa 25.x+)
- Star Emulator or Winlator to host the DXVK build

### Test Games
| Game | Engine | What It Tests |
|------|--------|---------------|
| Hollow Knight | Unity (D3D11) | Draw threshold correctness, GPLAsync stability |
| Tomb Raider (2013) | Crystal Dynamics (D3D11) | Swapchain, governor, draw batching |
| Subnautica | Unity (D3D11) | General stability, FSR |

### What to Monitor
```bash
adb logcat -s "DXVK" | grep -E "Vegas:|GetImage|tuneThreshold|compiler"
```

### Regression Checklist
- [ ] Game launches without crash
- [ ] `dxvk.enableStarProfile = False` disables all Vegas features
- [ ] `dxvk.enableAsync = False` disables async compilation (useful for debugging)
- [ ] No new Vulkan validation errors
- [ ] FPS and framepacing are not worse than previous build

---

## 7. Common Pitfalls

### "This is a backport, not a full DXVK upgrade"
- VEGAS 2.4.1 is based on DXVK v2.4.1, NOT v2.7.3
- The GPU BCn-to-ASTC transcoder from release-v2 is NOT present
- leegao's DxvkFence timeline semaphore is NOT present
- GPLAsync was backported from Ph42oN's patch, not from DXVK 2.7+ native GPL

### "Desktop Vulkan assumptions are invalid on Adreno"
- TBDR architecture hates unbounded draw batching — always cap thresholds
- Storage images on swapchain images will crash Turnip — use intermediate image
- Push constants are preferred over small UBOs
- Device-local memory is limited — watch allocation sizes

### "Static state is thread-unsafe"
- All baked state (`s_*` variables) is written once from `configure()` and
  read-only afterwards — no synchronization needed
- `thread_local` variables in `tuneThreshold()` and `analyzePerformance()`
  prevent cross-context interference

### "Config option namespaces"
- `dxvk.*` — DXVK core options (in `dxvk_options.cpp`)
- `dxvk.enableStarProfile` — VEGAS master switch
- `vegas.*` — VEGAS-specific options (tier override, etc.)

### "Low thresholds are intentional"
- Draw thresholds are {50, 150, 300} (not {100, 200, 350}) to fix 2D game
  pop-in/missing sprites. This does NOT affect rendering correctness — every
  draw call still renders, just with more frequent flushes.

---

## 8. CI Pipeline & WCP Dual Build

### Workflows

| Workflow | Trigger | Produces |
|----------|---------|----------|
| `build.yml` | `workflow_dispatch` | x64 + x32 DLLs (via MinGW cross-build) |
| `wcpbuild.yml` | `workflow_dispatch` | WCP packages from latest build artifact |

### WCP Dual Build

`wcpbuild.yml` builds **two** `.wcp` archives from the same DLLs:

| Artifact | Profile Type | Archive Name | Purpose |
|----------|-------------|--------------|---------|
| `wcp-dxvk-<sha>` | DXVK | `dxvk-2.4.1-vegas-<sha>.wcp` | Stock Winlator compatibility |
| `wcp-vegas-<sha>` | VEGAS | `vegas-2.4.1-<sha>.wcp` | Star Emulator |

### Workflow Security
- `build.yml` is build-only (no release creation, no external pushes)
- `wcpbuild.yml` has permissions: `actions: read, contents: read`
- No pushes to `vegas-releases` repo — user controls publication

---

## 9. Credits & Contributors

### GPLAsync Backport
The GPLAsync patch for DXVK 2.4 was created by **Ph42oN**
(commit `4e5658e97b6c` of `dxvk-gplasync`). Upstream GPLAsync by
**ishitatsuyuki** provided the async pipeline compilation foundation.

### Project Credits
- **Lead Developer:** isygold
- **Base Project:** DXVK v2.4.1 by doitsujin
- **FSR 1.0:** AMD GPUOpen (EASU compute shader)
- **License:** zlib/libpng

---

*Last updated: 2026-07-22 | Branch: build-fix-2.4.1*
