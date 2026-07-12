#pragma once

#include <cstdint>
#include <string>
#include <array>

namespace dxvk {

class DxvkDevice;
class DxvkAdapter;
class Config;

// -----------------------------------------------------------------------
//  VegasSessionReport  –  per-game-session data saved to JSON on exit
// -----------------------------------------------------------------------
struct VegasSessionReport {
  // Schema
  int32_t     schemaVersion    = 1;

  // Game identity
  std::string gameName;                           // "Hollow Knight.exe"
  std::string dxvkVersion;                        // DXVK_VERSION

  // Device
  std::string deviceName;                         // Vulkan device name
  std::string driverVersion;                      // Vulkan driver version string
  uint32_t    vendorId          = 0;
  uint32_t    deviceId          = 0;
  uint32_t    gpuTier           = 0;              // 0=unknown, 1=low, 2=mid, 3=high
  std::string gpuArch;                            // "tbdr" | "immediate"

  // Session metrics
  int64_t     durationSec       = 0;
  uint64_t    totalFrames       = 0;
  double      avgFps            = 0.0;
  double      minFps            = 9999.0;
  double      p1Fps             = 9999.0;
  bool        crashed           = false;

  // Effective config
  bool        vegasEnabled      = true;
  int32_t     drawThreshold     = 150;
  bool        bindSkip          = false;
  bool        tbdrMode          = false;
  bool        vramSwapApplied   = true;
  bool        gpuMaskApplied    = true;

  // User-facing rating (set by the app, not by the DLL)
  std::string rating;                             // "" | "smooth" | "okay" | "stuttery" | "crashed"

  // FPS histogram for p1 calculation (buckets of 5 FPS, 0..120+)
  static constexpr int kHistogramBuckets = 25;
  std::array<uint32_t, kHistogramBuckets> fpsHistogram = {};
};


class Vegas {
public:

  /// <summary>
  /// Reads dxvk.vegas.* options from the merged Config
  /// and applies them to the static control variables.
  /// Called once during DxvkInstance construction.
  /// </summary>
  static void configure(const Config& config);

  /// <summary>
  /// Returns true if the GPU is a TBDR (tile-based) architecture
  /// such as Adreno, Mali, or PowerVR.
  /// </summary>
  static bool isTbdrArch(const Config& config);

  /// <summary>
  /// Applies TBDR-aware optimisations to the Config object.
  /// Called after configure() when isTbdrArch() is true
  /// and the dxvk.vegas.tbdr option is not explicitly False.
  /// </summary>
  static void applyTbdrOptimizations(Config& config);

  static void applyVramSwap(Config& config);
  static void applyGpuMask(Config& config);

  // ── Session lifecycle ──────────────────────────────────────────

  /// <summary>
  /// Begins a tracking session.  Called once from DxvkDevice ctor.
  /// Creates the crash-marker file and checks for a prior crash.
  /// </summary>
  static void beginSession(const Config& config, DxvkAdapter* adapter);

  /// <summary>
  /// Ends the session and writes the JSON report to disk.
  /// Called from DxvkDevice destructor.
  /// </summary>
  static void endSession();

  /// <summary>
  /// Called once per Present() to count frames and sample FPS.
  /// Hooked in DxvkDevice::presentImage().
  /// </summary>
  static void onPresent();

  // ── Hot-path queries ───────────────────────────────────────────

  static bool shouldFlush(uint32_t drawCount);
  static bool shouldSkipBind();
  static bool isEnabled();

  // ── Static state ───────────────────────────────────────────────

  static bool                s_initialized;
  static bool                s_enabled;
  static bool                s_bindSkipEnabled;
  static uint32_t            s_tier;
  static uint32_t            s_drawThreshold;

  static VegasSessionReport s_report;             // accumulated during session
  static bool                s_sessionActive;
  static int64_t             s_sessionStartTick;  // hires performance counter
  static std::string         s_reportDir;         // directory for report files
  static std::string         s_markerPath;        // full path to crash-marker file
  static std::string         s_reportPath;        // full path to report JSON
};

}
