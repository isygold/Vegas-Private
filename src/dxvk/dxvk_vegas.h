#pragma once

#include <cstdint>

namespace dxvk {

class DxvkDevice;
class Config;

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

  static bool shouldFlush(uint32_t drawCount);
  static bool shouldSkipBind();
  static bool isEnabled();

  static bool     s_initialized;
  static bool     s_enabled;
  static bool     s_bindSkipEnabled;
  static uint32_t s_tier;
  static uint32_t s_drawThreshold;
};

}
