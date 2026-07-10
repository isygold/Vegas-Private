# 🎰 VEGAS Sarek — Adreno-Optimized DXVK-Sarek Port

[![Stars](https://img.shields.io/github/stars/isygold/Vegas-Private?style=social)](https://github.com/isygold/Vegas-Private)

A lightweight port of select **VEGAS** optimizations to **DXVK-Sarek**, purpose-built for **Adreno 610‑class GPUs** running under Star Emulator / Winlator on Android.

This branch (`1.11.1`) contains the complete source code — self-contained, no patching required. Build artifacts are published separately on the [releases page](https://github.com/isygold/vegas-releases/releases).

---

## 📦 Backported Features

| Feature | Description |
|---------|-------------|
| **Draw Threshold Governor** | Dynamically flushes the command buffer after N draws (tier-based: 100/200/350 for Adreno 610/640/700+). Reduces GPU pipeline stalls without sacrificing throughput. |
| **Dynamic VRAM Swap** | Reports `dxgi.maxDeviceMemory` as 40% of physical RAM, clamped to 1024–4096 MB. Prevents OOM crashes on devices with limited VRAM heap (~900 MiB usable on Adreno 610). |
| **GPU Persona Mask** | Spoofs Vendor/Device ID as an NVIDIA GPU (GTX 1050 Ti / GTX 1070 / RTX 3060 depending on Adreno tier). Tricks apps that blacklist unknown or mobile GPUs. |

No transcoder, no FSR, no framegen — just the optimizations that matter most for low-VRAM Adreno GPUs.

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

### Star Emulator
Download the `.wcp` package from the [releases page](https://github.com/isygold/vegas-releases/releases) and import it as type **VEGAS**.

### Manual install
Extract the DLLs from the WCP/archive and place them in your emulator's DXVK directory (typically `{storage}/emulated/0/StarEmulator/dxvk/`).

---

## 🔧 Build Instructions

This branch is designed to be built via **GitHub Actions** using the included workflow:

1. Go to the **Actions** tab → **Build DXVK (x64 + x32)**
2. Click **Run workflow**, select branch `1.11.1`
3. Wait ~5 minutes for both 64-bit and 32-bit builds to complete
4. Download the merged artifact (`dxvk-<sha>`)
5. DLLs are inside under `x64/` and `x32/`

The build uses a **Fedora 44** container with MinGW cross-compilers. It runs `meson setup` + `ninja install` directly for each architecture — no local toolchain needed.

### Local build (alternative)
If you prefer to build locally on Linux:

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

Pre-built releases are published on the **[vegas-releases](https://github.com/isygold/vegas-releases/releases)** repository.

---

## 📄 License

zlib/libpng — same as upstream DXVK.
