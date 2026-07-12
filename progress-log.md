# VEGAS / Star Emulator — Progress Log

---

## Initial Session — Build System & Release

### Summary
Established the build and release pipeline for VEGAS-Sarek on GitHub.
Configured cross-compilation from Fedora 40 (later switched to `release-v2`
workflows) with Mingw-w64 to produce `dxvk-1.11.1-vegas-sarek.tar.gz` /
`.zip` artifacts containing `x32/d3d9.dll`, `x64/d3d9.dll`,
`x64/d3d11.dll`, `x64/dxgi.dll`, and the supporting `dxvk.conf`.

### Files created/modified
- `.github/workflows/build.yml` — meson+ninja build for 64-bit DXVK
- `.github/workflows/wcpbuild.yml` — WCP packaging workflow (7za archive, two-part
  artifact upload)
- `dxvk.conf` — initial reference config

### Key decisions
- Use `release-v2` workflows (build.yml + wcpbuild.yml) over standalone Fedora 40
- Output dll extension `.dll` not `.so`
- Use `7za` (p7zip-full) not `7z`
- Build with direct meson setup + ninja, not `./package-release.sh`
- WCP packaging must handle cross-arch (x64 native, x32 via multiarch)

### Verification
- WCP packaging workflow tested end-to-end
- Hollow Knight ran stable with better framepacing than base Sarek
- Official release `v1.11.1-vegas-sarek` published on both `Vegas-Private`
  and `vegas-releases` repos

---

## Session 2 — README Overhaul & Build FAQ

### Summary
Replaced the minimal README with a comprehensive project page (badges,
feature table, expanded usage, troubleshooting) and created a dedicated
build FAQ (`VEGAS-DXVK-SAREK-BUILD-FAQ.md`) covering all known build
pitfalls and workflows.

### Files created/modified
- `README.md` — full rewrite with CI badges, feature comparison table,
  download/install/usage sections, troubleshooting, links to FAQ
- `VEGAS-DXVK-SAREK-BUILD-FAQ.md` — new file covering build prerequisites,
  meson setup, workflows, packaging, and common error resolution

### Key decisions
- Badges point to `Vegas-Private` (source) and `vegas-releases` (releases)
- FAQ cross-references GitHub workflow syntax for maintainability
- Both files included in release tarball via wcpbuild.yml

---

## 2026-07-12 — Option B: dxvk.vegas.* config + TBDR optimisations

### Summary
Implemented the second major feature set for VEGAS: user-configurable
`dxvk.vegas.*` options + automatic TBDR-aware optimisations for tile-based
GPUs (Adreno, Mali, PowerVR).  This addresses the overheating issues reported
by testers (Left 4 Dead, etc.) and gives users per-game control over every
VEGAS behaviour.

### Files modified
| File | Change |
|------|--------|
| `src/dxvk/dxvk_vegas.h` | Added `configure()`, `isTbdrArch()`, `applyTbdrOptimizations()`; removed unused `initializeProfile()` and `tuneThreshold()` |
| `src/dxvk/dxvk_vegas.cpp` | Full rework: removed dead code, implemented `configure()` (reads dxvk.vegas.* options), `isTbdrArch()` (sysfs + device-name detection), `applyTbdrOptimizations()` (depth prepass off, threshold uplift, async threads, frame latency) |
| `src/dxvk/dxvk_instance.cpp` | Calls `Vegas::configure(m_config)` after config merge; guards VRAM swap and GPU mask behind `dxvk.vegas.vramSwap`/`dxvk.vegas.gpuMask`; triggers TBDR path when appropriate |
| `dxvk.conf` | Complete overhaul — active per-game sections at top with VEGAS options for Unity, Source, CryEngine, RAGE, Creation, and racing engines; full commented DXVK reference at bottom |

### New config options (all namespaced under `dxvk.vegas.*`)
- `dxvk.vegas.enable` (bool, default True) — master on/off
- `dxvk.vegas.threshold` (int32, default 0 = auto) — draw-call flush threshold override
- `dxvk.vegas.vramSwap` (bool, default True) — clamp reported VRAM to ~40 % of RAM
- `dxvk.vegas.gpuMask` (Tristate, default Auto) — spoof NVIDIA GPU persona
- `dxvk.vegas.tbdr` (Tristate, default Auto) — TBDR-aware optimisations

### TBDR optimisations applied
1. Disables `d3d9.enableDepthPrePass` + `d3d11.enableDepthPrePass` (HSR in hardware)
2. Uplifts draw threshold by ~50 % (capped at 600) to batch more work per tile
3. Sets `dxvk.numAsyncThreads = 4` for better async compute on Adreno
4. Sets `dxvk.maxFrameLatency = 1` to reduce input lag on mobile

### Compatibility
- **Backward**: All changes are opt-in via `dxvk.conf`; default behaviour is identical
  to pre-Option-B code (auto-threshold from sysfs, VRAM swap + GPU mask on by default)
- **Config file**: `dxvk.conf` must be bundled with the release artifact
- **Games covered in dxvk.conf**: Hollow Knight, Subnautica, Celeste, L4D2, HL2, Portal 2,
  CS:S, Crysis + Warhead, GTA V, Skyrim, Oblivion, Fallout NV/3, NFS Most Wanted, NFS Carbon

---

## 2026-07-12 — User Feedback Collection System Design

### Summary
Designed a comprehensive three-layer user feedback collection system for
VEGAS game configuration data and wrote a detailed integration specification
as an HTML document for Banner (Bannerlator developer).

### Design decisions
- **Passive data generation, active sharing**: DLL writes report silently;
  user initiates share in the app
- **Three layers**: DLL (report generation) → App (share UI) → Backend (aggregation)
- **Funnel-first approach**: capture the 60% who'd tap a smiley, not just
  the 5% who write GitHub issues
- **Privacy by design**: no PII, no auto-upload without explicit opt-in consent
- **Layered rollout**: clipboard (Phase 1) → rating widget (Phase 2) →
  compatibility view (Phase 3) → auto-upload opt-in (Phase 4)

### File created
- `VEGAS-User-Feedback-System.html` — 668-line self-contained spec document
  covering JSON schema, screen wireframes, UI states, backend options,
  implementation roadmap, and privacy guidelines. Ready to send to Banner.

---

## 2026-07-12 — Phase 1 DLL Feedback: VegasSessionReport

### Summary
Implemented the DLL-side session report generation (Phase 1 of the feedback
system). Every DXVK session now produces a `<game>.vegas-report.json` file
next to the game executable and uses a `<game>.vegas-crash-marker` file for
crash detection.

### Files modified
| File | Change |
|------|--------|
| `src/dxvk/dxvk_vegas.h` | Added `VegasSessionReport` struct with GPU info, FPS histogram, config snapshot; added `beginSession()`, `endSession()`, `onPresent()` statics; added session tracking state |
| `src/dxvk/dxvk_vegas.cpp` | Implemented `beginSession()` (crash detection, marker write, device info, config snapshot, timer start), `endSession()` (FPS histogram → p1/avg, JSON writer, marker cleanup), `onPresent()` (frame counter, instant FPS → histogram) |
| `src/dxvk/dxvk_device.cpp` | Added `Vegas::beginSession()` at end of constructor, `Vegas::endSession()` at top of destructor, `Vegas::onPresent()` inside `presentImage()` |

### Report JSON schema (written to `<game>.vegas-report.json`)
```json
{
  "version": 1,
  "game": "Hollow Knight.exe",
  "dxvk": "1.11.1-vegas-sarek",
  "device": { "name", "driverVersion", "vendorId", "deviceId", "gpuTier", "gpuArch" },
  "session": { "durationSec", "totalFrames", "avgFps", "minFps", "p1Fps", "crashed" },
  "config": { "vegasEnabled", "drawThreshold", "bindSkip", "tbdrMode", "vramSwapApplied", "gpuMaskApplied" },
  "rating": null
}
```

### Crash detection
- A `.vegas-crash-marker` file is written at session start, deleted at clean exit
- If the marker persists to the next launch, the report's `crashed` field is set to `true`
- This detects: game crashes, power loss, force-kill, driver hangs

### FPS tracking
- `onPresent()` is called once per DxvkDevice::presentImage() (once per frame)
- Instant FPS computed from time between consecutive Present calls
- Bucketed into a 25-bin histogram (0-120+ FPS, 5 FPS per bucket)
- `endSession()` computes avg FPS (weighted) and 1st-percentile FPS from histogram
