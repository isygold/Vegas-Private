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

// ----------------------------------------------------------------
//  Option names (namespace dxvk.vegas.*)
// ----------------------------------------------------------------
static constexpr char kOptEnable[]    = "dxvk.vegas.enable";
static constexpr char kOptThreshold[] = "dxvk.vegas.threshold";
static constexpr char kOptVramSwap[]  = "dxvk.vegas.vramSwap";
static constexpr char kOptGpuMask[]   = "dxvk.vegas.gpuMask";
static constexpr char kOptTbdr[]      = "dxvk.vegas.tbdr";

// ----------------------------------------------------------------
//  configure()  –  called once from DxvkInstance ctor
// ----------------------------------------------------------------
void Vegas::configure(const Config& config) {
  // --- enable ---
  s_enabled = config.getOption<bool>(kOptEnable, true);

  if (!s_enabled) {
    Logger::info("Vegas: disabled by config (dxvk.vegas.enable = False)");
    return;
  }

  // --- threshold ---
  // 0 = auto-determine from GPU tier, otherwise override
  int32_t cfgThreshold = config.getOption<int32_t>(kOptThreshold, 0);
  if (cfgThreshold > 0) {
    s_drawThreshold = static_cast<uint32_t>(cfgThreshold);
    Logger::info(str::format("Vegas: threshold overridden to ", s_drawThreshold));
  } else {
    // Auto: detect tier from sysfs and pick threshold
    s_tier = detectGpuTierFromSysfs();
    static constexpr uint32_t autoThresholds[] = { 100, 200, 350 };
    s_drawThreshold = (s_tier >= 1 && s_tier <= 3)
      ? autoThresholds[s_tier - 1]
      : 150;
    Logger::info(str::format("Vegas: auto threshold=", s_drawThreshold,
      " (tier ", s_tier, ")"));
  }

  // --- bindSkip ---
  // Enable bind skipping on any Adreno-class GPU
  if (s_tier >= 1)
    s_bindSkipEnabled = true;

  Logger::info(str::format("Vegas: enabled=", s_enabled,
    " threshold=", s_drawThreshold,
    " bindSkip=", s_bindSkipEnabled,
    " tier=", s_tier));
}

// ----------------------------------------------------------------
//  isTbdrArch  –  detect tile-based GPU from Vulkan properties
// ----------------------------------------------------------------
bool Vegas::isTbdrArch(const Config& config) {
  // If the user explicitly opted out, respect that
  Tristate tbdrOpt = config.getOption<Tristate>(kOptTbdr, Tristate::Auto);
  if (tbdrOpt == Tristate::False)
    return false;

  // Check for known TBDR renderers via the GPU device name.
  // We read the string that was (potentially) set by our own
  // gpuMask.  If the mask is active the name says "NVIDIA …",
  // so we fall back to sysfs detection.
  if (detectGpuTierFromSysfs() >= 1)
    return true;

  // Also check the original Vulkan device name via
  // a well-known env var (set earlier by DxvkInstance).
  std::string name = config.getOption<std::string>("dxgi.customDeviceDesc", "");
  for (auto& c : name) c = std::tolower(c);
  if (name.find("adreno") != std::string::npos ||
      name.find("mali")   != std::string::npos ||
      name.find("powervr") != std::string::npos)
    return true;

  return tbdrOpt == Tristate::True; // only true if user forced it
}

// ----------------------------------------------------------------
//  applyTbdrOptimizations  –  tune DXVK for tile-based GPUs
// ----------------------------------------------------------------
void Vegas::applyTbdrOptimizations(Config& config) {
  // TBDR GPUs (Adreno, Mali, PowerVR) benefit from:
  //
  //  1. Disabling the depth pre-pass because they perform
  //     hidden surface removal (HSR) in hardware, making a
  //     separate depth-only pass redundant and wasteful.
  //
  //  2. Raising the draw-call threshold so that more draws
  //     are batched per tile, reducing tile-overhead.
  //
  //  3. Using the "Lazy" image layout strategy to avoid
  //     unnecessary render-pass transitions.
  //
  //  4. Pushing more state updates to the GPU in fewer,
  //     larger command buffers.

  Logger::info("Vegas: applying TBDR-aware optimizations");

  // --- Depth pre-pass ---
  // TBDR hardware does HSR at the tile level; a CPU-driven
  // depth pre-pass wastes bandwidth and increases power draw.
  config.setOption("d3d9.enableDepthPrePass",   "False");
  config.setOption("d3d11.enableDepthPrePass",  "False");

  // --- Draw threshold uplift ---
  // Batch more draws together to amortise tiling cost.
  // We boost the threshold by ~50 % over the current value.
  if (s_drawThreshold > 0 && s_drawThreshold < 600) {
    uint32_t boosted = s_drawThreshold + (s_drawThreshold / 2);
    if (boosted > 600) boosted = 600;
    s_drawThreshold = boosted;
    Logger::info(str::format("Vegas: TBDR threshold uplifted to ", s_drawThreshold));
  }

  // --- Async presentation & relaxed sync ---
  // Adreno drivers handle presentation better when not
  // strictly synchronised to vblank boundaries.
  config.setOption("dxvk.numAsyncThreads",       "4");

  // --- Latency reduction ---
  // Lower the number of queued frames to reduce
  // input lag on mobile GPUs.
  config.setOption("dxvk.maxFrameLatency",       "1");

  Logger::info("Vegas: TBDR optimisations applied");
}

// ----------------------------------------------------------------
//  applyVramSwap  –  clamp reported VRAM to ~40 % of system RAM
// ----------------------------------------------------------------
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

// ----------------------------------------------------------------
//  applyGpuMask  –  spoof NVIDIA GPUs to avoid low-quality paths
// ----------------------------------------------------------------
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

// ----------------------------------------------------------------
//  Query helpers  –  used by dxvk_context.cpp hot-paths
// ----------------------------------------------------------------
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
