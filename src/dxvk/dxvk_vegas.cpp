#include "dxvk_vegas.h"
#include "dxvk_device.h"
#include "dxvk_adapter.h"

#include "../util/config/config.h"
#include "../util/log/log.h"
#include "../util/util_env.h"

#include <version.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <unistd.h>
#include <chrono>

namespace dxvk {

// ── Static member definitions ────────────────────────────────────
bool                Vegas::s_initialized     = false;
bool                Vegas::s_enabled         = true;
bool                Vegas::s_bindSkipEnabled = false;
uint32_t            Vegas::s_tier            = 0;
uint32_t            Vegas::s_drawThreshold   = 150;
VegasSessionReport  Vegas::s_report;
bool                Vegas::s_sessionActive   = false;
int64_t             Vegas::s_sessionStartTick = 0;
std::string         Vegas::s_reportDir;
std::string         Vegas::s_markerPath;
std::string         Vegas::s_reportPath;


// ── Internal helpers ─────────────────────────────────────────────

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
  uint32_t tier = 2;
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


/// Returns monotonic tick count (microseconds since unspecified epoch).
static int64_t nowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}


/// Returns directory portion of a full path.
static std::string dirnameOf(const std::string& path) {
  auto pos = path.find_last_of("/\\");
  return (pos != std::string::npos) ? path.substr(0, pos) : ".";
}


/// Derive GPU arch string from device name.
static const char* detectGpuArch(DxvkAdapter* adapter) {
  if (!adapter) return "unknown";
  const auto& props = adapter->deviceProperties();
  std::string name(props.deviceName);
  for (auto& c : name) c = std::tolower(c);
  if (name.find("adreno") != std::string::npos ||
      name.find("mali")   != std::string::npos ||
      name.find("powervr") != std::string::npos)
    return "tbdr";
  return "immediate";
}


// ── Option names ─────────────────────────────────────────────────
static constexpr char kOptEnable[]    = "dxvk.vegas.enable";
static constexpr char kOptThreshold[] = "dxvk.vegas.threshold";
static constexpr char kOptVramSwap[]  = "dxvk.vegas.vramSwap";
static constexpr char kOptGpuMask[]   = "dxvk.vegas.gpuMask";
static constexpr char kOptTbdr[]      = "dxvk.vegas.tbdr";


// ── configure() ──────────────────────────────────────────────────
void Vegas::configure(const Config& config) {
  s_enabled = config.getOption<bool>(kOptEnable, true);
  if (!s_enabled) {
    Logger::info("Vegas: disabled by config (dxvk.vegas.enable = False)");
    return;
  }

  int32_t cfgThreshold = config.getOption<int32_t>(kOptThreshold, 0);
  if (cfgThreshold > 0) {
    s_drawThreshold = static_cast<uint32_t>(cfgThreshold);
    Logger::info(str::format("Vegas: threshold overridden to ", s_drawThreshold));
  } else {
    s_tier = detectGpuTierFromSysfs();
    static constexpr uint32_t autoThresholds[] = { 100, 200, 350 };
    s_drawThreshold = (s_tier >= 1 && s_tier <= 3)
      ? autoThresholds[s_tier - 1]
      : 150;
    Logger::info(str::format("Vegas: auto threshold=", s_drawThreshold,
      " (tier ", s_tier, ")"));
  }

  if (s_tier >= 1)
    s_bindSkipEnabled = true;

  Logger::info(str::format("Vegas: enabled=", s_enabled,
    " threshold=", s_drawThreshold,
    " bindSkip=", s_bindSkipEnabled,
    " tier=", s_tier));
}

// ── beginSession() ────────────────────────────────────────────────
void Vegas::beginSession(const Config& config, DxvkAdapter* adapter) {
  // Already tracking?
  if (s_sessionActive) return;
  s_sessionActive = true;

  // Determine directory next to the game executable
  std::string exePath = env::getExePath();
  s_reportDir = dirnameOf(exePath);
  std::string exeName = env::getExeName();

  // Paths — visible convention (no leading dot, vegas- prefix)
  s_markerPath = s_reportDir + "/vegas-" + exeName + ".marker.txt";
  s_reportPath = s_reportDir + "/vegas-" + exeName + ".report.json";

  // ── Crash detection ──────────────────────────────────────────
  // If the marker from a previous session still exists, that
  // session ended abnormally (crash / power loss / force-kill).
  {
    FILE* marker = std::fopen(s_markerPath.c_str(), "r");
    if (marker) {
      std::fclose(marker);
      s_report.crashed = true;
      Logger::warn(str::format("Vegas: prior session crashed (marker: ",
        s_markerPath, ")"));
    }
  }

  // Backward compat: remove old-format marker files so they don't accumulate
  std::remove((s_reportDir + "/" + exeName + ".vegas-crash-marker").c_str());

  // Write current-session marker
  {
    FILE* m = std::fopen(s_markerPath.c_str(), "w");
    if (m) {
      std::fprintf(m, "%lld\n", static_cast<long long>(nowUs()));
      std::fclose(m);
    }
  }

  // ── Device info ──────────────────────────────────────────────
  s_report.gameName     = exeName;
  s_report.dxvkVersion  = DXVK_VERSION;

  if (adapter) {
    const auto& props = adapter->deviceProperties();
    s_report.deviceName  = props.deviceName;
    s_report.vendorId    = props.vendorID;
    s_report.deviceId    = props.deviceID;

    // Format driver version as major.minor.patch
    uint32_t dv = props.driverVersion;
    s_report.driverVersion = str::format(
      VK_VERSION_MAJOR(dv), ".",
      VK_VERSION_MINOR(dv), ".",
      VK_VERSION_PATCH(dv));

    // Detect TBDR arch from real device name
    bool isTbdr = (detectGpuArch(adapter) == std::string("tbdr"));
    s_report.gpuArch = isTbdr ? "tbdr" : "immediate";
  }

  // GPU tier (already set by configure())
  s_report.gpuTier = s_tier;

  // ── Effective config ─────────────────────────────────────────
  s_report.vegasEnabled    = s_enabled;
  s_report.drawThreshold   = s_drawThreshold;
  s_report.bindSkip        = s_bindSkipEnabled;
  s_report.tbdrMode        = isTbdrArch(config);
  // vramSwap/gpuMask: read the user-facing options (they were applied earlier)
  s_report.vramSwapApplied = config.getOption<bool>(kOptVramSwap, true);
  Tristate gm = config.getOption<Tristate>(kOptGpuMask, Tristate::Auto);
  s_report.gpuMaskApplied  = (gm != Tristate::False);

  // ── Timer start ──────────────────────────────────────────────
  s_sessionStartTick = nowUs();

  Logger::info(str::format("Vegas: session started for ", exeName));
}


// ── endSession() ─────────────────────────────────────────────────
void Vegas::endSession() {
  if (!s_sessionActive) return;

  int64_t elapsedUs = nowUs() - s_sessionStartTick;
  s_report.durationSec = elapsedUs / 1'000'000;

  // Compute avg FPS and 1st-percentile from histogram
  uint64_t histTotal = 0;
  for (auto c : s_report.fpsHistogram)
    histTotal += c;

  if (histTotal > 0) {
    // Average FPS = weighted average of bucket midpoints
    double sum = 0.0;
    uint64_t p1Target = static_cast<uint64_t>(std::ceil(histTotal * 0.01));
    uint64_t running = 0;
    bool p1Found = false;

    for (size_t b = 0; b < s_report.fpsHistogram.size(); b++) {
      double fpsLow  = static_cast<double>(b) * 5.0;
      double fpsHigh = fpsLow + 5.0;
      double mid     = (b == s_report.fpsHistogram.size() - 1) ? 125.0 : (fpsLow + fpsHigh) / 2.0;

      sum += mid * static_cast<double>(s_report.fpsHistogram[b]);

      if (!p1Found) {
        running += s_report.fpsHistogram[b];
        if (running >= p1Target) {
          s_report.p1Fps = mid;
          p1Found = true;
        }
      }
    }

    s_report.avgFps = sum / static_cast<double>(histTotal);
  } else {
    s_report.avgFps = 0.0;
    s_report.p1Fps  = 0.0;
  }

  // If minFps wasn't touched, set to avg as fallback
  if (s_report.minFps > 9990.0)
    s_report.minFps = s_report.avgFps;
  if (s_report.p1Fps > 9990.0)
    s_report.p1Fps = s_report.avgFps;

  // ── Write GitHub Issue markdown ─────────────────────────────
  {
    std::string issuePath = s_reportDir + "/vegas-" + s_report.gameName + ".issue.md";

    FILE* m = std::fopen(issuePath.c_str(), "w");
    if (m) {
      std::fprintf(m, "## VEGAS Game Report\n\n");
      std::fprintf(m, "**Game**: %s\n", s_report.gameName.c_str());
      std::fprintf(m, "**Device**: %s &middot; Tier %u &middot; %s\n",
        s_report.deviceName.c_str(), s_report.gpuTier, s_report.gpuArch.c_str());
      std::fprintf(m, "**DXVK**: %s\n", s_report.dxvkVersion.c_str());
      std::fprintf(m, "**Session**: %.0f fps avg &middot; %.0f fps p1 &middot; %llds &middot; %s\n",
        s_report.avgFps, s_report.p1Fps,
        static_cast<long long>(s_report.durationSec),
        s_report.crashed ? "crashed" : "no crash");
      std::fprintf(m, "\n### Config\n");
      std::fprintf(m, "| Option | Value |\n");
      std::fprintf(m, "|--------|-------|\n");
      std::fprintf(m, "| dxvk.vegas.enable | %s |\n",  s_report.vegasEnabled ? "True" : "False");
      std::fprintf(m, "| dxvk.vegas.threshold | %u |\n", s_report.drawThreshold);
      std::fprintf(m, "| dxvk.vegas.vramSwap | %s |\n",  s_report.vramSwapApplied ? "True" : "False");
      std::fprintf(m, "| dxvk.vegas.gpuMask | %s |\n",   s_report.gpuMaskApplied ? "Auto" : "False");
      std::fprintf(m, "| dxvk.vegas.tbdr | %s |\n",      s_report.tbdrMode ? "Auto" : "False");
      std::fprintf(m, "\n### Experience\n");
      std::fprintf(m, "<!-- 😊 Smooth / 🤷 Okay / 😵 Stuttery / 💀 Crashed -->\n");
      std::fprintf(m, "\n### Notes\n");
      std::fprintf(m, "<!-- What did you tweak? What worked? What didn't? -->\n");
      std::fclose(m);
    }
  }

  // ── Write JSON report ────────────────────────────────────────
  FILE* f = std::fopen(s_reportPath.c_str(), "w");
  if (!f) {
    Logger::warn(str::format("Vegas: cannot write report to ", s_reportPath));
    // Remove marker so we don't false-detect a crash next session
    if (std::remove(s_markerPath.c_str()) != 0)
      Logger::warn(str::format("Vegas: failed to remove marker ", s_markerPath));
    s_sessionActive = false;
    return;
  }

  std::fprintf(f, "{\n");
  std::fprintf(f, "  \"version\" : %d,\n", s_report.schemaVersion);
  std::fprintf(f, "  \"game\"    : \"%s\",\n",  s_report.gameName.c_str());
  std::fprintf(f, "  \"dxvk\"    : \"%s\",\n",  s_report.dxvkVersion.c_str());
  // device
  std::fprintf(f, "  \"device\" : {\n");
  std::fprintf(f, "    \"name\"          : \"%s\",\n", s_report.deviceName.c_str());
  std::fprintf(f, "    \"driverVersion\" : \"%s\",\n", s_report.driverVersion.c_str());
  std::fprintf(f, "    \"vendorId\"      : \"0x%x\",\n", s_report.vendorId);
  std::fprintf(f, "    \"deviceId\"      : \"0x%x\",\n", s_report.deviceId);
  std::fprintf(f, "    \"gpuTier\"       : %u,\n",       s_report.gpuTier);
  std::fprintf(f, "    \"gpuArch\"       : \"%s\"\n",    s_report.gpuArch.c_str());
  std::fprintf(f, "  },\n");
  // session
  std::fprintf(f, "  \"session\" : {\n");
  std::fprintf(f, "    \"durationSec\" : %lld,\n", static_cast<long long>(s_report.durationSec));
  std::fprintf(f, "    \"totalFrames\" : %llu,\n", static_cast<unsigned long long>(s_report.totalFrames));
  std::fprintf(f, "    \"avgFps\"      : %.1f,\n", s_report.avgFps);
  std::fprintf(f, "    \"minFps\"      : %.1f,\n", s_report.minFps);
  std::fprintf(f, "    \"p1Fps\"       : %.1f,\n", s_report.p1Fps);
  std::fprintf(f, "    \"crashed\"     : %s\n",    s_report.crashed ? "true" : "false");
  std::fprintf(f, "  },\n");
  // config
  std::fprintf(f, "  \"config\" : {\n");
  std::fprintf(f, "    \"vegasEnabled\"    : %s,\n", s_report.vegasEnabled ? "true" : "false");
  std::fprintf(f, "    \"drawThreshold\"   : %u,\n", s_report.drawThreshold);
  std::fprintf(f, "    \"bindSkip\"        : %s,\n", s_report.bindSkip ? "true" : "false");
  std::fprintf(f, "    \"tbdrMode\"        : %s,\n", s_report.tbdrMode ? "true" : "false");
  std::fprintf(f, "    \"vramSwapApplied\" : %s,\n", s_report.vramSwapApplied ? "true" : "false");
  std::fprintf(f, "    \"gpuMaskApplied\"  : %s\n",  s_report.gpuMaskApplied ? "true" : "false");
  std::fprintf(f, "  },\n");
  // rating (always null — app overwrites)
  std::fprintf(f, "  \"rating\" : null\n");
  std::fprintf(f, "}\n");

  std::fclose(f);

  // ── Remove crash marker (clean exit) ─────────────────────────
  if (std::remove(s_markerPath.c_str()) != 0)
    Logger::warn(str::format("Vegas: failed to remove marker ", s_markerPath));

  Logger::info(str::format("Vegas: report written to ", s_reportPath,
    " (", s_report.durationSec, "s, ", s_report.totalFrames, " frames, ",
    s_report.avgFps, " avg fps)"));

  s_sessionActive = false;
}


// ── onPresent()  –  called per frame from DxvkDevice::presentImage
void Vegas::onPresent() {
  if (!s_sessionActive) return;

  int64_t now = nowUs();
  s_report.totalFrames++;

  // Compute instant FPS from time since last call
  static int64_t lastPresentUs = 0;
  if (lastPresentUs != 0) {
    double dtSec = static_cast<double>(now - lastPresentUs) / 1'000'000.0;
    if (dtSec > 0.0) {
      double instantFps = 1.0 / dtSec;
      if (instantFps < s_report.minFps)
        s_report.minFps = instantFps;

      // Clamp for histogram
      if (instantFps > 120.0) instantFps = 120.0;
      size_t bucket = static_cast<size_t>(instantFps / 5.0);
      if (bucket >= s_report.fpsHistogram.size())
        bucket = s_report.fpsHistogram.size() - 1;
      s_report.fpsHistogram[bucket]++;
    }
  }
  lastPresentUs = now;
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
