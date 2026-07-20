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

---

## 2026-07-12 — GitHub Issues as single destination + .vegas-github-issue.md

### Summary
Shifted the feedback system architecture to a single destination model:
all reports land exclusively on `github.com/isygold/vegas-releases/issues`.
Updated both the HTML spec and the DLL to reflect this.

### Changes
- **HTML spec** (`/sdcard/VEGAS-User-Feedback-System.html`): Rewrote Philosophy tenets
  (user authority, crash-triggered reporting, single destination); simplified architecture
  diagram to show GitHub Issues as the only backend; redesigned app UI section with crash
  notification dialog; removed Google Sheets / custom webhook options; updated flow diagram.
- **DLL** (`dxvk_vegas.cpp`): Added `writeIssueBody()` that generates
  `<game>.vegas-github-issue.md` alongside the JSON report — a pre-formatted GitHub issue
  with device info, config table, and placeholder fields for experience/notes.

### Files on disk after a game session
```
<game-dir>/
├── <game>.exe.vegas-crash-marker      (temporary, deleted on clean exit)
├── <game>.exe.vegas-report.json       (machine-readable)
└── <game>.exe.vegas-github-issue.md   (GitHub issue body, ready to paste)
```

### Commit: `926aa5e`

---

## 2026-07-12 — README update with full documentation

### Summary
Updated the 1.11.1 branch README to document all new features:
configuration system, TBDR optimizations, session reports, and per-game profiles.

### New sections added
- **📦 VEGAS Features** — expanded table with Config Overrides, TBDR, Per-Game Profiles, Session Reports
- **⚙️ Configuration** — all 5 `dxvk.vegas.*` options with types/defaults, per-game syntax with examples
- **🌡️ TBDR Optimizations** — explains the 4 automatic changes and when they activate
- **📊 Session Reports** — documents the 3 local files and how to submit them to GitHub Issues

### Commit: `3299d2a`

---

## 2026-07-12 — Full Build Verification

### Summary
Both GitHub Actions workflows executed successfully on commit `3299d2a`:
- **Build DXVK (x64 + x32)** — compiled without errors all modified files
- **WCP Packaging** — produced release artifacts with dxvk.conf, README, FAQ

### Commits on 1.11.1 (in order)
| Commit | Description |
|--------|-------------|
| `8d1c219` | Option B: dxvk.vegas.* config + TBDR optimisations |
| `a4d4b23` | Phase 1: VegasSessionReport — DLL-side session tracking |
| `926aa5e` | Add .vegas-github-issue.md generation, update HTML spec |
| `3299d2a` | Update README: config docs, TBDR, session reports, per-game profiles |

### Release (corrected naming)
- Created release [`v1.11.2-3299d2a`](https://github.com/isygold/vegas-releases/releases/tag/v1.11.2-3299d2a) on vegas-releases
- Assets: `vegas-1.11.2-3299d2a.wcp`, `dxvk-1.11.2-3299d2a.wcp`, `dxvk.conf`, `VEGAS-DXVK-SAREK-BUILD-FAQ.html`
- Same release published identically on both vegas-releases and Vegas-Private
- Version bumped from 1.11.1 to 1.11.2 in the build

---

## FUTURE FEATURE — Release Card in Bannerlator / Star Emulator

### Problem
Users install a new VEGAS WCP but have no way of knowing what changed.
They may miss important updates (new config options, TBDR fixes, per-game
profiles).  An in-app card should appear after an update to inform them.

### Two-part solution

#### Part A: `release.json` manifest inside the WCP (DLL-side)

Add a file `release.json` to the WCP package so the app can read release
notes without a network call.  Generated at build time.

**Schema** (`release.json`):
```json
{
  "version": "1.11.2-3299d2a",
  "released": "2026-07-12",
  "title": "VEGAS Sarek v1.11.2",
  "notes": [
    "New dxvk.vegas.* config options — tune every behaviour without rebuilding",
    "TBDR optimisations for Adreno/Mali/PowerVR — cooler running, less heat",
    "Per-game profiles for Unity, Source, CryEngine, RAGE, Creation Engine",
    "Session reports (.vegas-report.json + .vegas-github-issue.md)",
    "Updated dxvk.conf with 15+ game presets"
  ],
  "configChanged": true,
  "docsUrl": "https://github.com/isygold/vegas-releases/releases/tag/v1.11.2-3299d2a"
}
```

**Where to add in the build**:
- In `wcpbuild.yml`, after the WCP archive is created, inject `release.json`
  into it using `7za a <wcp-file> release.json`
- The `release.json` content can be generated from the workflow context
  (version from `meson.project_version()`, date from `date -u +%Y-%m-%d`,
  notes from a static file or variable)

**App reads it**:
- When a WCP is imported/installed, Bannerlator extracts `release.json`
  from the archive
- If the version differs from the previously installed version, show a card

#### Part B: GitHub API polling (optional enhancement)

For the "new version available" badge or notification when the user hasn't
updated yet:

```
GET https://api.github.com/repos/isygold/vegas-releases/releases/latest
```

Response contains `tag_name`, `body` (release notes markdown), `html_url`.

**App logic**:
1. On startup (or periodic), fetch the latest release tag
2. Compare against installed version
3. If newer, show a subtle badge: "🆕 v1.11.2 available"
4. No action required — just an informational indicator

**Rate limits**: Unauthenticated GitHub API allows 60 requests/hour.
Authenticated (via the user's PAT, if stored) allows 5000/hour.
For just a version check once per session, 60/hr is plenty.

#### App UI: The Card

Two states:

**State 1 — After update (release.json in WCP)**:
```
┌──────────────────────────────────────────────────────┐
│  🆕  What's New in VEGAS 1.11.2                      │
│                                                      │
│  ✓ New dxvk.vegas.* config options                   │
│  ✓ TBDR optimisations — cooler gaming                │
│  ✓ Per-game profiles for 15+ titles                  │
│  ✓ Session reports for issue reporting               │
│                                                      │
│           [📄 Full Notes]  [✕ Dismiss]               │
└──────────────────────────────────────────────────────┘
```
- Shown once after version change
- `[📄 Full Notes]` opens the release URL in browser
- `[✕ Dismiss]` marks as read (store version in SharedPreferences)

**State 2 — New version available (GitHub API)**:
```
┌──────────────────────────────────────────────────────┐
│  🆕  VEGAS 1.11.3 Available                          │
│  A new version is out. Tap to see what's new.        │
│                                                      │
│           [📄 Release Notes]  [✕]                    │
└──────────────────────────────────────────────────────┘
```
- Shown subtly in the VEGAS settings panel, not obtrusive
- Dismiss persists until next version bump

#### Bannerlator implementation notes (for Banner)

**Detection timing**:
- `release.json` check: during WCP install (`ContentsManager` or
  `extractDxWrapperFiles`)
- GitHub API check: on VEGAS settings page open, or once per day via
  `WorkManager`

**Storage**:
- `SharedPreferences` key: `vegas_last_seen_version` (string)
- On card dismiss: write the current version tag
- On next install: compare, show if different

**Dismiss behaviour**:
- One dismiss per version — same version never shows again
- Cross-version: if user dismisses 1.11.2 then installs 1.11.3, show again

### Implementation order
1. Add `release.json` generation to `wcpbuild.yml` (simplest, no app changes)
2. Banner reads `release.json` on install and shows card
3. (Optional) GitHub API check for "new version available" badge

### Notes
- The release card content comes from the DLL/WCP side, not hardcoded in the app
- This keeps the app generic — you update the notes by updating the build,
  not by pushing an APK update

---

## 2026-07-12 — AI Translation Plan for Non-English GitHub Issues

### Decision
Instead of using a rigid translation API (Google Translate, DeepL), use an
**AI model (LLM)** for translating non-English GitHub issues. An LLM handles
nuance, context, and technical domain terms far better than traditional
translation APIs.

### Three implementation options

#### A. 💬 Manual forwarding (immediate, zero setup)
- User pastes any non-English issue link or text into this conversation
- AI reads, translates, and helps craft a response in the original language
- **Pros**: Highest accuracy (AI understands VEGAS technical context), no infrastructure
- **Cons**: Manual — user must copy the issue here
- **Status**: Already how Issue #15 ("Questão") was handled

#### B. 🤖 GitHub Action with AI API key
- Workflow auto-detects non-English issues and posts a translation comment
- Detects language, adds label (`lang-pt`, `lang-es`, etc.)
- Posts: "🔄 Translated from Portuguese: [English text]"
- When user replies in English, Action translates back and tags the issue author
- **Effort**: ~2 hours to write workflow; needs an API key (OpenAI / Anthropic)
- **Cost**: GPT-4o-mini ~$0.15/MTok → ~2500 issues per dollar

#### C. 🧠 Hybrid — Action notifies, AI translates on demand
- Light Action that:
  1. Detects non-English issue
  2. Posts a minimal comment: "Issue detected in Portuguese. @isygold will respond shortly."
  3. Sends webhook or just the user is informed
- Actual translation is handled here in conversation (best of both worlds)

### Recommendation
**Start with A (manual), build B when volume grows.** Current volume is zero
active non-English issues.

---

## 2026-07-12 — Release Naming Correction + Dual-Repo Consistency

### Problem
The v1.11.2-3299d2a release used wrong asset naming:
- `VEGAS-1.11.2-3299d2a.wcp` → should be `vegas-1.11.2-3299d2a.wcp` (lowercase `vegas-`)
- `DXVK-1.11.2-3299d2a.wcp` → should be `dxvk-1.11.2-3299d2a.wcp` (lowercase `dxvk-`)
- Missing `VEGAS-DXVK-SAREK-BUILD-FAQ.html` on both repos
- WCPs only on vegas-releases, missing from Vegas-Private

### Fix performed
1. **Deleted** both releases and tags from vegas-releases and Vegas-Private
2. **Re-created** both releases with identical content:
   - Release name: `VEGAS Sarek v1.11.2-3299d2a`
   - Tag: `v1.11.2-3299d2a`
3. **Uploaded** identical assets to both:
   - `vegas-1.11.2-3299d2a.wcp` (35 MB)
   - `dxvk-1.11.2-3299d2a.wcp` (35 MB)
   - `dxvk.conf` (12 KB)
   - `VEGAS-DXVK-SAREK-BUILD-FAQ.html` (1.7 KB)

### Naming convention confirmed
| Type | Pattern | Example |
|------|---------|---------|
| VEGAS wrapper | `vegas-{version}-{hash}.wcp` | `vegas-1.11.2-3299d2a.wcp` |
| DXVK wrapper | `dxvk-{version}-{hash}.wcp` | `dxvk-1.11.2-3299d2a.wcp` |
| Config | `dxvk.conf` | — |
| FAQ | `VEGAS-DXVK-SAREK-BUILD-FAQ.html` | — |

### Verification
Both releases verified identical via GitHub API. Release URLs:
- https://github.com/isygold/vegas-releases/releases/tag/v1.11.2-3299d2a
- https://github.com/isygold/Vegas-Private/releases/tag/v1.11.2-3299d2a

---

## 2026-07-13 — Marker Persistence Bugfix + Visible File Naming

### Bug found
In `endSession()`, if the JSON report file could not be written (permissions,
weird path, disk issue), the function returned early WITHOUT removing the crash
marker. On the next launch, the code would find the orphaned marker and falsely
set `crashed = true`.

### Fix applied (commit `7a4fb1c`)
1. **Marker removal in failure path**: Added `std::remove(s_markerPath)` before
   the early return when JSON write fails, with a logged warning on failure.
2. **Error logging on success path**: Changed the existing `std::remove` call
   to check its return value and log a warning if it fails.
3. **Backward compat cleanup**: `beginSession()` now also removes old-format
   marker files (`<game>.vegas-crash-marker`) so they don't accumulate after
   the naming change.

### Naming change (all 3 files)
| File | Old (hidden dotfile) | New (visible prefix) |
|------|---------------------|---------------------|
| Crash marker | `{game}.vegas-crash-marker` | `vegas-{game}.marker.txt` |
| Session report | `{game}.vegas-report.json` | `vegas-{game}.report.json` |
| GitHub issue | `{game}.vegas-github-issue.md` | `vegas-{game}.issue.md` |

Also removed a stale line in the issue markdown section where `issuePath` was
assigned twice.

---

## 2026-07-13 — HUD Branding + Version Bump

### Changes
- **HUD label**: `dxvk_hud_item.cpp` line 94 changed from
  `"DXVK-Sarek " DXVK_VERSION` to `"VEGAS Sarek " DXVK_VERSION`
- **Version string**: `version.h.in` changed from `"v1.11.0"` to `"1.11.2"`
  (clean version, no `v` prefix)

### Result
In-game HUD now shows: **VEGAS Sarek 1.11.2** instead of `DXVK-Sarek v1.11.0`.

### Full commit log
| Hash | Description |
|------|-------------|
| `7a4fb1c` | Fix marker cleanup bug + rename VEGAS files to visible naming |
| `2db8e6b` | Update HUD branding to VEGAS Sarek |
| `05e0ed7` | Bump version to 1.11.2 |

---

## PENDING / NEXT STEPS (ready to resume)

### High priority
1. **Trigger new build** — Run build.yml + wcpbuild.yml to produce WCP with all recent changes (HUD branding, marker fix, file rename, version bump)
2. **Build release.json generation** into `wcpbuild.yml` — so the WCP carries release notes for the in-app "What's New" card

### Medium priority
3. **GitHub Action for issue translation** — auto-detect non-English issues and post translation comment (AI Translation Plan, Option B)

### Low priority / optional
4. **GitHub API polling** in Bannerlator for "new version available" badge
5. Any new feature requests from testing or community feedback
