#pragma once

#include <cstdint>

namespace dxvk {

class DxvkDevice;
class Config;

class Vegas {
public:
  static void initializeProfile(uint32_t& threshold, bool& enabled, bool& bindSkip, uint32_t& tier, DxvkDevice* device);

  static void tuneThreshold(uint32_t& threshold, float load, float frameTime, uint32_t tier);

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
