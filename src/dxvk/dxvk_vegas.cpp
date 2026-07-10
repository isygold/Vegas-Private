#include "dxvk_vegas.h"
#include "dxvk_device.h"
#include "dxvk_adapter.h"

#include "../util/config/config.h"
#include "../util/log/log.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace dxvk {

bool     Vegas::s_initialized     = false;
bool     Vegas::s_enabled         = true;
bool     Vegas::s_bindSkipEnabled = false;
uint32_t Vegas::s_tier            = 0;
uint32_t Vegas::s_drawThreshold   = 150;

static uint32_t classifyAdrenoTier(const char* name) {
  const char* p = std::strstr(name, "Adreno");
  if (!p) p = std::strstr(name, "adreno");
  if (!p) return 0;

  while (*p && !std::isdigit(*p)) ++p;
  if (!*p) return 0;

  unsigned long model = std::strtoul(p, nullptr, 10);

  if (model >= 700) return 3;
  if (model >= 640) return 2;
  if (model >= 610) return 1;
  return 0;
}

void Vegas::initializeProfile(uint32_t& threshold, bool& enabled, bool& bindSkip, uint32_t& tier, DxvkDevice* device) {
  if (!device || device->adapter() == nullptr) return;

  // Check for Adreno GPU via device name
  const auto& props = device->adapter()->deviceProperties();
  std::string name(props.deviceName);
  for (auto& c : name) c = std::tolower(c);
  bool isAdreno = name.find("adreno") != std::string::npos;

  if (isAdreno) {
    enabled   = true;
    bindSkip  = true;
    tier      = classifyAdrenoTier(props.deviceName);

    static constexpr uint32_t defaultThresholds[] = { 100, 200, 350 };
    threshold = (tier >= 1 && tier <= 3) ? defaultThresholds[tier - 1] : 100;

    Logger::info(str::format("Vegas: Adreno tier ", tier,
      " threshold=", threshold, " bindSkip=", bindSkip));
  }
}

void Vegas::tuneThreshold(uint32_t& threshold, float load, float frameTime, uint32_t tier) {
  static constexpr uint32_t baseThresholds[] = { 100, 200, 350 };
  uint32_t base = (tier >= 1 && tier <= 3) ? baseThresholds[tier - 1] : 100;

  static constexpr float capMultipliers[] = { 2.0f, 2.0f, 1.7f };
  float multiplier = (tier >= 1 && tier <= 3) ? capMultipliers[tier - 1] : 2.0f;
  uint32_t cap = static_cast<uint32_t>(base * multiplier);

  if (load > 0.90f && frameTime > 25.0f) {
    // GPU-bound — batch more draws
    threshold = cap;
  } else if (load < 0.40f && frameTime > 12.0f) {
    // CPU-bound — flush more frequently
    threshold = std::max(50u, base / 2);
  } else {
    threshold = base;
  }
}

static uint32_t detectGpuTierFromSysfs() {
  uint32_t tier = 2; // mid-range default
  FILE* f = std::fopen("/sys/class/kgsl/kgsl-3d0/gpu_model", "r");
  if (!f) f = std::fopen("/sys/class/kgsl/kgsl-3d0/devfreq/device/gpu_model", "r");
  if (!f) return tier;

  char buf[64] = { 0 };
  if (std::fgets(buf, int(sizeof(buf)), f)) {
    const char* p = buf;
    while (*p && !std::isdigit(*p)) ++p;
    if (*p) {
      unsigned long model = std::strtoul(p, nullptr, 10);
      if (model >= 700)      tier = 3;
      else if (model >= 640) tier = 2;
      else                   tier = 1;
    }
  }
  std::fclose(f);
  return tier;
}

void Vegas::applyVramSwap(Config& config) {
#ifdef _WIN32
  MEMORYSTATUSEX statex;
  statex.dwLength = sizeof(statex);
  GlobalMemoryStatusEx(&statex);
  uint64_t totalRamMB = statex.ullTotalPhys / (1024 * 1024);
#else
  long pages    = sysconf(_SC_PHYS_PAGES);
  long pageSize = sysconf(_SC_PAGE_SIZE);
  uint64_t totalRamMB = static_cast<uint64_t>(pages)
                      * static_cast<uint64_t>(pageSize)
                      / (1024ULL * 1024ULL);
#endif

  uint32_t vramReport = static_cast<uint32_t>(totalRamMB * 0.40f);
  if (vramReport < 1024) vramReport = 1024;
  if (vramReport > 4096) vramReport = 4096;

  config.setOption("dxgi.maxDeviceMemory", std::to_string(vramReport));
  config.setOption("dxgi.maxSharedMemory",  std::to_string(vramReport / 2));
}

void Vegas::applyGpuMask(Config& config) {
  uint32_t tier = detectGpuTierFromSysfs();

  static constexpr struct { const char* vid; const char* did; } personaTable[4] = {
    {},                     // [0] unused
    { "10de", "1c82" },    // [1] GTX 1050 Ti
    { "10de", "1b81" },    // [2] GTX 1070
    { "10de", "2503" },    // [3] RTX 3060
  };

  if (tier >= 1 && tier <= 3) {
    config.setOption("dxgi.customVendorId",   personaTable[tier].vid);
    config.setOption("dxgi.customDeviceId",   personaTable[tier].did);
    config.setOption("dxgi.customDeviceDesc",
      std::string("NVIDIA GeForce (Vegas - Tier ") + std::to_string(tier) + ")");
  }
}

bool Vegas::shouldFlush(uint32_t drawCount) {
  return s_enabled && drawCount >= s_drawThreshold;
}

bool Vegas::shouldSkipBind() {
  return s_enabled && s_bindSkipEnabled;
}

bool Vegas::isEnabled() {
  return s_enabled;
}

} // namespace dxvk
