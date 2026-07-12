> **📖 [Read the Build FAQ first →](./VEGAS-DXVK-SAREK-BUILD-FAQ.md)**
> Important info about which file to download, VEGAS+VKD3D auto-install, and Unity compatibility.
<br>

# 🎰 VEGAS Sarek — Adreno-Optimized DXVK-Sarek Port

[![Stars](https://img.shields.io/github/stars/isygold/Vegas-Private?style=social)](https://github.com/isygold/Vegas-Private)
[![Sponsor](https://img.shields.io/badge/Sponsor-%24?logo=github&style=social)](https://github.com/sponsors/isygold)

A lightweight port of select **VEGAS** optimizations to **DXVK-Sarek**, purpose-built for **Adreno 610‑class GPUs** running under Star Emulator / Winlator on Android.

This branch (`1.11.1`) contains the complete source code — self-contained, no patching required. Build artifacts are published on the [releases page](https://github.com/isygold/Vegas-Private/releases).

---

## 📦 VEGAS Features

| Feature | Description |
|---------|-------------|
| **Draw Threshold Governor** | Dynamically flushes the command buffer after N draws (tier-based: 100/200/350 for Adreno 610/640/700+). Reduces GPU pipeline stalls without sacrificing throughput. |
| **Dynamic VRAM Swap** | Reports `dxgi.maxDeviceMemory` as 40% of physical RAM, clamped to 1024–4096 MB. Prevents OOM crashes on devices with limited VRAM heap (~900 MiB usable on Adreno 610). |
| **GPU Persona Mask** | Spoofs Vendor/Device ID as an NVIDIA GPU (GTX 1050 Ti / GTX 1070 / RTX 3060 depending on Adreno tier). Tricks apps that blacklist unknown or mobile GPUs. |
| **Config Overrides** | Every VEGAS behaviour can be enabled/disabled/tuned via `dxvk.vegas.*` options in `dxvk.conf` — no rebuild needed. |
| **TBDR Optimizations** | Automatic tile-based GPU detection (Adreno, Mali, PowerVR) with depth prepass disable, draw threshold uplift, and latency reduction for reduced heat & power draw. |
| **Per-Game Profiles** | Built-in optimised presets for Unity, Source, CryEngine, RAGE, Creation, and racing engine titles. |
| **Session Reports** | Automatic `.vegas-report.json` generation with FPS, crash detection, and GPU info. Ready to submit as a GitHub issue. |

No transcoder, no FSR, no framegen — just the optimizations that matter most for low-VRAM Adreno GPUs.

---

## ⚙️ Configuration

VEGAS is controlled via the standard DXVK configuration file — place a `dxvk.conf` next to your game executable or set the `DXVK_CONFIG_FILE` environment variable.

### `dxvk.vegas.*` Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `dxvk.vegas.enable` | bool | `True` | Master switch. Set to `False` to disable all VEGAS optimizations. |
| `dxvk.vegas.threshold` | int32 | `0` (auto) | Draw-call flush threshold. `0` = auto-detect from GPU tier. Set to `100`–`600` to override. |
| `dxvk.vegas.vramSwap` | bool | `True` | Report VRAM as ~40% of system RAM. Prevents OOM in VRAM-hungry titles. |
| `dxvk.vegas.gpuMask` | Tristate | `Auto` | Spoof NVIDIA GPU persona. `Auto` = enable on Adreno, `False` = disable, `True` = force. |
| `dxvk.vegas.tbdr` | Tristate | `Auto` | TBDR-aware optimizations. `Auto` = enable on tile-based GPUs, `False` = disable, `True` = force. |

### Per-Game Configuration

Create sections matching the game executable name:

```ini
[Hollow Knight.exe]
dxvk.vegas.threshold = 0
dxvk.vegas.vramSwap  = True
d3d9.maxFrameRate    = 60

[left4dead2.exe]
dxvk.vegas.enable    = True
dxvk.vegas.threshold = 0
d3d9.maxFrameRate    = 60
d3d9.presentInterval = 1

[GTA5.exe]
dxvk.vegas.threshold = 0
dxvk.vegas.vramSwap  = True
dxvk.vegas.gpuMask   = Auto
d3d9.maxFrameRate    = 60
```

If a game has compatibility issues, disable VEGAS for that title only:

```ini
[ProblematicGame.exe]
dxvk.vegas.enable = False
```

A complete reference `dxvk.conf` with presets for Unity, Source, CryEngine, RAGE, Creation Engine, and racing games is included in this repository.

---

## 🌡️ TBDR-Aware Optimizations

On tile-based GPUs (Adreno, Mali, PowerVR), VEGAS automatically applies:

1. **Depth prepass disabled** — TBDR hardware performs hidden surface removal at the tile level. A CPU-driven depth prepass wastes bandwidth and increases power draw.
2. **Draw threshold uplifted ~50%** — batches more work per tile, reducing tile-overhead overhead and improving GPU utilisation.
3. **Async compute threads = 4** — better async compute pipeline utilisation on mobile GPUs.
4. **Frame latency = 1** — reduces queued frame pressure and input lag.

These activate automatically when the GPU is detected as tile-based (`dxvk.vegas.tbdr = Auto`). They can be forced on or off via the config.

---

## 📊 Session Reports

Every game session produces a report file next to the executable — **no data is sent anywhere unless you explicitly choose to submit it**.

### Files created

| File | When | Purpose |
|------|------|---------|
| `<game>.vegas-crash-marker` | During play | Deleted on clean exit. If it persists, the session crashed. |
| `<game>.vegas-report.json` | On exit | Machine-readable: GPU info, FPS histogram, config snapshot, crash flag. |
| `<game>.vegas-github-issue.md` | On exit | Pre-formatted GitHub issue body ready to copy & paste. |

### How to report an issue

1. Play the game through VEGAS
2. If you encounter a crash or performance issue, attach the `.vegas-report.json` and `.vegas-github-issue.md` files to a new issue at [vegas-releases/issues](https://github.com/isygold/vegas-releases/issues)
3. Or open the `.vegas-github-issue.md` — it's already formatted as a complete bug report, just paste it in

This gives me your exact GPU model, config, FPS data, and crash status — no guesswork.

---

## 🧱 Base & Credits

| Role | Author |
|------|--------|
| **Base** | [`zeyadadev/DXVK-Sarek`](https://github.com/zeyadadev/DXVK-Sarek) tag `v1.11.1-mali-fix` |
| **Original DXVK** | [doitsujin/dxvk](https://github.com/doitsujin/dxvk) (Philip Rebohle) |
| **Original Sarek** | [pythonlover02/DXVK-Sarek](https://github.com/pythonlover02/DXVK-Sarek) |
| **VEGAS backport & integration** | [@isygold](https://github.com/isygold) |

Thanks to [Blisto91](https://github.com/Blisto91), [AmerXz](https://github.com/AmerXz), [Gcenx](https://github.com/Gcenx), [WinterSnowfall](https://github.com/WinterSnowfall) for their contributions to the Sarek project.

---

## ⚠️ Status

**This release is under active refinement.** The in-game HUD overlay and version string still reflect the base Sarek version — these will be updated in a future release to properly identify this build as VEGAS Sarek.

Use at your own risk on non-Adreno hardware. Feedback and issue reports are welcome.

---

## 🚀 Usage

### Star Emulator / Bannerlator
Download the **VEGAS-prefixed** `.wcp` package from the [vegas-releases page](https://github.com/isygold/vegas-releases/releases) and import it as type **VEGAS**, or install via the emulator directly in the **VEGAS+DXVK wrapper** configuration.

### Other emulators (WinNative, GameHub, BBoxHub, Winlator forks, etc.)
Download the **DXVK-prefixed** `.wcp` package from the [Vegas-Private page](https://github.com/isygold/Vegas-Private/releases) and import it as type **DXVK**. This works with any emulator that supports standard DXVK WCP packages.

> [Vegas-Private](https://github.com/isygold/Vegas-Private) is the central repository for all VEGAS releases, build structures, and source code. Both `.wcp` variants (VEGAS-type and DXVK-type) are published there for each release.

---

## 🔧 Build Instructions

### Via GitHub Actions (recommended)

1. Go to the **Actions** tab → **Build DXVK (x64 + x32)** (or **WCP Packaging** for a complete release archive)
2. Click **Run workflow**, select branch `1.11.1`
3. Wait ~5 minutes for the build to complete
4. Download the artifact:
   - `dxvk-<sha>` — raw DLLs under `x64/` and `x32/`
   - `VEGAS-prefixed` or `DXVK-prefixed` `.wcp` — ready-to-import package

### Local build (Linux)

```bash
# Install dependencies (Fedora)
sudo dnf install mingw64-gcc-c++ mingw64-winpthreads-static \
  mingw32-gcc-c++ mingw32-winpthreads-static \
  glslang meson ninja-build pkgconf

# 64-bit
meson setup build64 --cross-file build-win64.txt --buildtype release \
  --prefix "$PWD/build" --bindir x64 --libdir x64 -Db_ndebug=if-release
ninja -C build64 install

# 32-bit
meson setup build32 --cross-file build-win32.txt --buildtype release \
  --prefix "$PWD/build" --bindir x32 --libdir x32 -Db_ndebug=if-release
ninja -C build32 install
```

Requirements: wine 7.1+, Meson 0.49+, MinGW-w64 10.0+, glslang.

---

## ⚙️ Environment Variables

All standard DXVK environment variables work. Key ones for this fork:

| Variable | Description |
|---|---|
| `DXVK_HUD=1` | Shows GPU name, FPS, and frame time |
| `DXVK_FRAME_RATE=60` | Caps FPS to 60 |
| `DXVK_STATE_CACHE=0` | Disables state cache |
| `DXVK_ALL_CORES=1` | Uses all CPU cores for shader compilation |
| `DXVK_FILTER_DEVICE_NAME="Adreno"` | Forces a specific Vulkan device |

See the [upstream DXVK README](https://github.com/doitsujin/dxvk) for the full list.

---

## 📥 Downloads

Pre-built releases are published on the **[releases page](https://github.com/isygold/Vegas-Private/releases)** of this repository.

---

## 📄 License

zlib/libpng - updated.
