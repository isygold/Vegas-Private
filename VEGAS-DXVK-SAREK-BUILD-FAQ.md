# 🎰 VEGAS & VEGAS DXVK Sarek — Which file do I download?

> **Quick answer:** Both `.wcp` packages contain **the same DLLs**. The only difference is the `profile.json` tag telling your emulator what *type* of wrapper it is. Pick the one your emulator expects below.

---

## 🤔 What is VEGAS?

**VEGAS** is a community-driven fork of **DXVK** (and **DXVK-Sarek**) optimized for **Qualcomm Adreno GPUs** on Android emulators (Star Emulator, Winlator, WinNative, GameHub, BBoxHub, etc.).

It adds a **Draw Threshold Governor** (reduces stutter), **Dynamic VRAM Swap** (prevents OOM crashes), and **GPU Persona Mask** (spoofs an NVIDIA GPU so apps don't blacklist you). These are backported to the lightweight **Sarek 1.x** codebase.

> **⚠️ Note:** This build is primarily a **supportive fix for Unity-based games**. It may **not run properly** on non-Unity titles. Performance and compatibility with other engines varies.

---

## ⚡ VEGAS+VKD3D Auto-Install

**VEGAS+VKD3D** comes with a built-in configuration that **automatically pulls available versions** directly from **GitHub** when you browse the version list inside the emulator. Just pick the version you want and it installs — no manual download needed.

This feature is available on **Star Emulator** and **Bannerlator** *only*. For all other emulators, download the appropriate `.wcp` file manually using the guide below.

---

## 📦 Which file do I pick?

### 🎰 VEGAS-prefixed `vegas-*.wcp`

**Tagged for:** Emulators that distinguish between VEGAS and DXVK wrapper types.

| Use for | How |
|---|---|
| **Star Emulator** | Import as type **VEGAS** |
| **Bannerlator** | Install via **VEGAS+DXVK wrapper** config |

---

### 🔷 DXVK-prefixed `dxvk-*.wcp`

**Tagged for:** Emulators that expect a standard DXVK type. **Same DLLs** as the VEGAS package.

| Use for | How |
|---|---|
| **WinNative** | Import as type **DXVK** |
| **GameHub** | Import as type **DXVK** |
| **BBoxHub** | Import as type **DXVK** |
| **Winlator (all forks)** | Import as type **DXVK** |
| **Any other emulator** | Import as type **DXVK** |

---

## ⚡ Quick Decision

| Emulator | File to download |
|---|---|
| Star Emulator / Bannerlator | `vegas-*.wcp` (type VEGAS) |
| WinNative | `dxvk-*.wcp` (type DXVK) |
| GameHub | `dxvk-*.wcp` (type DXVK) |
| BBoxHub | `dxvk-*.wcp` (type DXVK) |
| Winlator (all forks) | `dxvk-*.wcp` (type DXVK) |
| Anything else | `dxvk-*.wcp` (type DXVK) |

---

## ❓ FAQ

### Are the DLLs inside both files different?
**No.** The `.dll` files are **identical** in both packages. The only difference is the `profile.json` inside the `.wcp` — it tells your emulator what **type** of wrapper this is (VEGAS vs DXVK). Pick the one your emulator expects.

### What if my emulator isn't listed above?
Use the **DXVK-prefixed** (`dxvk-*.wcp`) package and import it as type **DXVK**. This is the safe default for any Android emulator that supports custom Wrappers/DXVK.

### Where do I download these files?
Both files are published on the [Vegas-Private releases page](https://github.com/isygold/Vegas-Private/releases) and the [vegas-releases page](https://github.com/isygold/vegas-releases/releases). Look for the tag **v1.11.1-vegas-sarek**.

### I installed the wrong one — will it break things?
Unlikely. The DLLs are the same, so your game will run fine either way. The only issue is that your emulator might not recognize the wrapper type correctly if you use the wrong `.wcp`. Just download the correct one and swap it.

---

**[⬆ Back to README](./README.md)**
