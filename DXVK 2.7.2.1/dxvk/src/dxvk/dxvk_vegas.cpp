#include "dxvk_vegas.h"
#include "dxvk_device.h"
#include "dxvk_adapter.h"
#include "../util/config/config.h"

#include <algorithm>
#include <string>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace dxvk {

  // ============================================================
  // Baked state — all values determined by initializeProfile()
  // ============================================================
  bool     Vegas::s_initialized    = false;
  bool     Vegas::s_enabled        = false;
  bool     Vegas::s_bindSkipEnabled = false;
  uint32_t Vegas::s_tier           = 0;
  uint32_t Vegas::s_drawThreshold  = 150;
  uint32_t Vegas::s_haaeThreshold  = 65;



  void Vegas::initializeProfile(uint32_t& threshold, bool& enabled, bool& bindSkip, uint32_t& tier, DxvkDevice* device) {
      if (device == nullptr || device->adapter() == nullptr) return;
      auto& props = device->adapter()->deviceProperties().core.properties;

      static constexpr const char* adrenoStr = "adreno";
      static constexpr size_t adrenoLen = 6;
      bool isAdreno = false;

      if (device->adapter()->isAdreno()) {
          isAdreno = true;
      } else {
          for (size_t i = 0; i < VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - adrenoLen && props.deviceName[i] != '\0'; ++i) {
              size_t j = 0;
              for (; j < adrenoLen; ++j) {
                  if (std::tolower(static_cast<unsigned char>(props.deviceName[i + j])) != adrenoStr[j]) break;
              }
              if (j == adrenoLen) {
                  isAdreno = true;
                  break;
              }
          }
      }

      if (isAdreno) {
          enabled = true;
          bindSkip = true;
          tier = device->adapter()->getStarEnginePersona();
          // VEGAS: Set default tier-based threshold (fallback)
          static constexpr uint32_t defaultThresholds[] = {600, 1200, 2000};
          threshold = (tier >= 1 && tier <= 3) ? defaultThresholds[tier - 1] : 600;
      }
  }

  void Vegas::initializeProfile(uint32_t& threshold, bool& enabled, bool& bindSkip, uint32_t& tier, DxvkDevice* device, bool isD3D9) {
      if (device == nullptr) return;
      initializeProfile(threshold, enabled, bindSkip, tier, device);

      if (isD3D9 && enabled) {
          static constexpr uint32_t d3d9ThresholdTable[] = {1500, 3000, 5000};
          if (tier >= 1 && tier <= 3) {
              threshold = d3d9ThresholdTable[tier - 1];
          } else {
              threshold = 1500;
          }
      }
  }

  // VEGAS: Governor-style tiered threshold (AdrenoGovernor logic)
  void Vegas::tuneThreshold(uint32_t& threshold, float load, float frameTime, uint32_t tier) {
      if (tier == 1) {
          if (load > 0.90f && frameTime > 25.0f)
              threshold = 1200;
          else if (load < 0.60f)
              threshold = 8000;
      } else if (tier == 2) {
          if (load > 0.93f && frameTime > 29.0f)
              threshold = 1600;
          else if (load < 0.64f)
              threshold = 8000;
      } else {
          if (load > 0.95f && frameTime > 33.0f)
              threshold = 2000;
          else if (load < 0.70f)
              threshold = 8000;
      }
  }

  // VEGAS: ZeroInitShaders = 1 (always enable for Unity/Adreno stability)
  bool Vegas::shouldZeroInit(uint32_t tier) {
      return true;
  }


  void Vegas::calculateAspectRatio(uint32_t w, uint32_t h, float& outX, float& outY) {
    if (w == 0 || h == 0) { outX = 1.0f; outY = 1.0f; return; }
    constexpr float targetRatio = 16.0f / 9.0f;
    float currentRatio = static_cast<float>(w) / static_cast<float>(h);

    if (currentRatio > targetRatio) {
        outX = targetRatio / currentRatio;
        outY = 1.0f;
    } else if (currentRatio < targetRatio) {
        outX = 1.0f;
        outY = currentRatio / targetRatio;
    } else {
        outX = 1.0f;
        outY = 1.0f;
    }
  }

  uint64_t Vegas::getSystemRamMB() {
#ifdef _WIN32
      MEMORYSTATUSEX statex;
      statex.dwLength = sizeof(statex);
      GlobalMemoryStatusEx(&statex);
      return statex.ullTotalPhys / (1024 * 1024);
#else
      static const long pageSize = sysconf(_SC_PAGE_SIZE);
      long pages = sysconf(_SC_PHYS_PAGES);
      return static_cast<uint64_t>(pages) * static_cast<uint64_t>(pageSize) / (1024ULL * 1024ULL);
#endif
  }

  // ============================================================
  // Config-load-time overloads (self-aware, no Vk device needed)
  // ============================================================

  void Vegas::applyVramSwap(Config& config) {
    uint64_t totalRamMB = getSystemRamMB();
    uint32_t vramReport = static_cast<uint32_t>(totalRamMB * 0.40);

    // Safety bounds: 1 GB min, 4 GB max
    if (vramReport < 1024)  vramReport = 1024;
    if (vramReport > 4096)  vramReport = 4096;

    config.setOption("dxgi.maxDeviceMemory", std::to_string(vramReport));
    config.setOption("dxgi.maxSharedMemory",  std::to_string(vramReport / 2));
  }


  void Vegas::applyGpuMask(Config& config) {
    // Self-aware: detect GPU tier without Vk device.
    // Tries Android sysfs; falls back to safe default.
    uint32_t tier = 2; // mid-range default (GTX 1070)

    FILE* f = std::fopen("/sys/class/kgsl/kgsl-3d0/gpu_model", "r");
    if (!f) f = std::fopen("/sys/class/kgsl/kgsl-3d0/devfreq/device/gpu_model", "r");

    if (f) {
      char buf[64] = {0};
      if (std::fgets(buf, int(sizeof(buf)), f)) {
        // Find and parse the Adreno model number
        const char* p = buf;
        while (*p && !std::isdigit(static_cast<unsigned char>(*p))) ++p;
        if (*p) {
          unsigned long model = std::strtoul(p, nullptr, 10);
          if (model >= 700)       tier = 3; // Adreno 7xx/8xx -> RTX 3060
          else if (model >= 640)  tier = 2; // Adreno 640-660 -> GTX 1070
          else                    tier = 1; // Adreno 610/619 -> GTX 1050 Ti
        }
      }
      std::fclose(f);
    }

    // Apply persona based on detected tier
    static constexpr struct { const char* vid; const char* did; } personaTable[4] = {
      {},                                          // [0] unused
      {"10de", "1c82"}, // [1] GTX 1050 Ti
      {"10de", "1b81"}, // [2] GTX 1070
      {"10de", "2503"}, // [3] RTX 3060
    };

    if (tier >= 1 && tier <= 3) {
      config.setOption("dxgi.customVendorId", personaTable[tier].vid);
      config.setOption("dxgi.customDeviceId", personaTable[tier].did);
      config.setOption("dxgi.customDeviceDesc",
        std::string("NVIDIA GeForce (Vegas - Tier ") + std::to_string(tier) + ")");
    }
  }


  void Vegas::applyVramSwap(VkPhysicalDeviceMemoryProperties& props, uint32_t tier) {
    uint64_t systemRamBytes = getSystemRamMB() * 1024ULL * 1024ULL;

    // VEGAS: Ratio-based VRAM swap scaled to actual device RAM
    static constexpr float vramRatioTable[] = { 0.25f, 0.33f, 0.40f };
    float ratio = (tier >= 1 && tier <= 3) ? vramRatioTable[tier - 1] : 0.25f;

    uint64_t extraVram = static_cast<uint64_t>(systemRamBytes * ratio);
    extraVram = std::clamp(extraVram, 1ULL << 30, 8ULL << 30);

    uint64_t maxSafeVram = systemRamBytes / 2ULL;

    for (uint32_t i = 0; i < props.memoryHeapCount; i++) {
        if (props.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            uint64_t newSize = props.memoryHeaps[i].size + extraVram;
            props.memoryHeaps[i].size = std::min(newSize, maxSafeVram);
        }
    }
  }

  void Vegas::applyGpuMask(VkPhysicalDeviceProperties& props, uint32_t persona) {
       struct PersonaConfig {
           uint32_t vendorID;
           uint32_t deviceID;
           const char* deviceName;
       };

       static constexpr PersonaConfig personas[] = {
           {0, 0, ""},
           {0x10DE, 0x1C82, "NVIDIA GeForce GTX 1050 Ti (Vegas)"},
           {0x10DE, 0x2184, "NVIDIA GeForce GTX 1660 (Vegas)"},
           {0x10DE, 0x2520, "NVIDIA GeForce RTX 3060 Laptop GPU (Vegas)"}
       };

       if (persona >= 1 && persona <= 3) {
           const auto& config = personas[persona];
           props.vendorID = config.vendorID;
           props.deviceID = config.deviceID;
           std::strncpy(props.deviceName, config.deviceName, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1);
           props.deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1] = '\0';
       }
  }

  // VEGAS: Enhanced LSFG with tiered frame time thresholds
  bool Vegas::needsFrameGen(float frameTime, uint32_t tier) {
      if (tier == 1) return false;
      if (tier == 2) return frameTime > 29.0f;
      return frameTime > 33.0f;
  }

  void Vegas::calculateFsrConstants(VegasFsrConstants& c, VkExtent3D src, VkExtent3D dst) {
      c.info[0] = static_cast<float>(src.width)  / static_cast<float>(dst.width);
      c.info[1] = static_cast<float>(src.height) / static_cast<float>(dst.height);
      c.info[2] = 0.5f * c.info[0] - 0.5f;
      c.info[3] = 0.5f * c.info[1] - 0.5f;
  }


  VegasPerformanceState Vegas::analyzePerformance(float load, float frameTime, float targetFrameTime) {
      thread_local float s_prevFrameTime = 16.6f;
      float delta = std::abs(frameTime - s_prevFrameTime);
      s_prevFrameTime = frameTime;

      // Adaptive thresholds relative to target frame time
      // targetFrameTime = 1000.0 / fpsLimit; default 16.667ms = 60 FPS
      float laggingThreshold   = targetFrameTime * 1.5f;
      float overheatThreshold  = targetFrameTime * 3.0f;

      if (load >= 0.95f && frameTime >= overheatThreshold) {
          return VegasPerformanceState::Overheating;
      }
      if (delta > targetFrameTime * 0.75f) {
          return VegasPerformanceState::Stuttering;
      }
      if (frameTime >= laggingThreshold) {
          return VegasPerformanceState::Lagging;
      }
      return VegasPerformanceState::Normal;
  }

  uint32_t Vegas::getGraphColor(VegasPerformanceState state) {
      static constexpr uint32_t colorTable[] = {
          0x00FF00,
          0xFFFF00,
          0xFF8800,
          0xFF0000
      };
      return colorTable[static_cast<int>(state)];
  }


  const char* Vegas::getStatusString(VegasPerformanceState state) {
      static constexpr const char* statusTable[] = {
          "NORMAL",
          "LAGGING",
          "STUTTERING",
          "OVERHEATING"
      };
      return statusTable[static_cast<int>(state)];
  }

  // VEGAS: ASTC helpers — used by the gated shouldTranscodeFormat/transcodeImageData
  bool Vegas::formatIsBcn(VkFormat format) {
    switch (format) {
      case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
      case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
      case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
      case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
      case VK_FORMAT_BC2_UNORM_BLOCK:
      case VK_FORMAT_BC2_SRGB_BLOCK:
      case VK_FORMAT_BC3_UNORM_BLOCK:
      case VK_FORMAT_BC3_SRGB_BLOCK:
      case VK_FORMAT_BC4_UNORM_BLOCK:
      case VK_FORMAT_BC4_SNORM_BLOCK:
      case VK_FORMAT_BC5_UNORM_BLOCK:
      case VK_FORMAT_BC5_SNORM_BLOCK:
      case VK_FORMAT_BC6H_UFLOAT_BLOCK:
      case VK_FORMAT_BC6H_SFLOAT_BLOCK:
      case VK_FORMAT_BC7_UNORM_BLOCK:
      case VK_FORMAT_BC7_SRGB_BLOCK:
        return true;
      default:
        return false;
    }
  }

  VkFormat Vegas::getAstcFormat(VkFormat bcnFormat) {
    switch (bcnFormat) {
      case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
      case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
      case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
      case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        return VK_FORMAT_ASTC_6x6_UNORM_BLOCK;

      case VK_FORMAT_BC2_UNORM_BLOCK:
        return VK_FORMAT_ASTC_5x5_UNORM_BLOCK;
      case VK_FORMAT_BC2_SRGB_BLOCK:
        return VK_FORMAT_ASTC_5x5_SRGB_BLOCK;

      case VK_FORMAT_BC3_UNORM_BLOCK:
        return VK_FORMAT_ASTC_5x5_UNORM_BLOCK;
      case VK_FORMAT_BC3_SRGB_BLOCK:
        return VK_FORMAT_ASTC_5x5_SRGB_BLOCK;

      case VK_FORMAT_BC4_UNORM_BLOCK:
      case VK_FORMAT_BC4_SNORM_BLOCK:
        return VK_FORMAT_ASTC_6x6_UNORM_BLOCK;

      case VK_FORMAT_BC5_UNORM_BLOCK:
      case VK_FORMAT_BC5_SNORM_BLOCK:
        return VK_FORMAT_ASTC_5x5_UNORM_BLOCK;

      case VK_FORMAT_BC6H_UFLOAT_BLOCK:
      case VK_FORMAT_BC6H_SFLOAT_BLOCK:
        return VK_FORMAT_UNDEFINED;

      case VK_FORMAT_BC7_UNORM_BLOCK:
        return VK_FORMAT_ASTC_5x5_UNORM_BLOCK;
      case VK_FORMAT_BC7_SRGB_BLOCK:
        return VK_FORMAT_ASTC_5x5_SRGB_BLOCK;

      default:
        return VK_FORMAT_UNDEFINED;
    }
  }

  // GATED FEATURE — see comment above transcodeImageData() for rationale.
  // Currently logs when a texture would benefit from ASTC but does NOT modify
  // the format. Flip the switch in createImage() only after the upload-path
  // transcoding pipeline is wired AND block-size alignment is verified on
  // real Adreno 6xx/7xx hardware.
  VkFormat Vegas::shouldTranscodeFormat(
      VkFormat              originalFormat,
      VkImageUsageFlags     usage,
      VkExtent3D            extent,
      const Rc<DxvkAdapter>& adapter) {
    if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_STORAGE_BIT))
      return VK_FORMAT_UNDEFINED;

    uint64_t pixelCount = uint64_t(extent.width) * extent.height * std::max(extent.depth, 1u);
    uint64_t totalBytes = pixelCount * 4;
    if (totalBytes < (512u * 1024u))
      return VK_FORMAT_UNDEFINED;

    if (!formatIsBcn(originalFormat))
      return VK_FORMAT_UNDEFINED;

    if (originalFormat == VK_FORMAT_BC6H_UFLOAT_BLOCK ||
        originalFormat == VK_FORMAT_BC6H_SFLOAT_BLOCK)
      return VK_FORMAT_UNDEFINED;

    VkFormat astcFormat = getAstcFormat(originalFormat);
    if (astcFormat == VK_FORMAT_UNDEFINED)
      return VK_FORMAT_UNDEFINED;

    VkFormatFeatureFlags2 features = adapter->getFormatFeatures(astcFormat).optimal;
    if (!(features & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT))
      return VK_FORMAT_UNDEFINED;

    return astcFormat;
  }

  // ============================================================
  // CPU-side BCn->ASTC transcoder (GATED — NOT ACTIVATED YET)
  // ============================================================
  //
  // Rationale: On older Adreno 6xx GPUs the closed-source Qualcomm
  // driver decodes BCn in software at draw time, causing mid-frame
  // CPU stalls. Pre-transcoding BCn→ASTC at upload time moves that
  // decode cost to loading where it's harmless. On Adreno 8xx with
  // native BCn hardware this buys nothing — but Star Engine targets
  // the full range of Android devices.
  //
  // Why it's still gated:
  //   1. Block-size mismatch — BCn uses 4×4 blocks, ASTC uses 5×5
  //      or 6×6. Most game textures (power-of-2) don't align, so
  //      vkCmdCopyBufferToImage would fail validation.
  //   2. Upload pipeline — the staging buffer contains BCn data;
  //      format-swapping the VkImage without also transcoding the
  //      pixel data produces garbage.
  //   3. Untested — written speculatively, never run end-to-end.
  //
  // To activate:
  //   a) In DxvkDevice::createImage(): swap createInfo.format to
  //      the ASTC format returned by shouldTranscodeFormat().
  //   b) In DxvkContext::uploadImage[Fb|Hw](): call
  //      transcodeImageData() on the staging buffer before the
  //      vkCmdCopyBufferToImage call.
  //   c) Verify block-size alignment on real Adreno 6xx/7xx hw.
  //
  // Until then this entire section is dead code — kept for future
  // bring-up.
  // ============================================================

  namespace {

    void decodeBC1(uint8_t dst[16][4], const uint8_t src[8]) {
      uint16_t c0 = src[0] | (uint16_t(src[1]) << 8);
      uint16_t c1 = src[2] | (uint16_t(src[3]) << 8);

      uint8_t r[4], g[4], b[4];
      r[0] = uint8_t((c0 >> 11) * 255 / 31);
      g[0] = uint8_t(((c0 >> 5) & 0x3F) * 255 / 63);
      b[0] = uint8_t((c0 & 0x1F) * 255 / 31);
      r[1] = uint8_t((c1 >> 11) * 255 / 31);
      g[1] = uint8_t(((c1 >> 5) & 0x3F) * 255 / 63);
      b[1] = uint8_t((c1 & 0x1F) * 255 / 31);

      if (c0 > c1) {
        r[2] = (2 * r[0] + r[1]) / 3; g[2] = (2 * g[0] + g[1]) / 3; b[2] = (2 * b[0] + b[1]) / 3;
        r[3] = (r[0] + 2 * r[1]) / 3; g[3] = (g[0] + 2 * g[1]) / 3; b[3] = (b[0] + 2 * b[1]) / 3;
      } else {
        r[2] = (r[0] + r[1]) / 2; g[2] = (g[0] + g[1]) / 2; b[2] = (b[0] + b[1]) / 2;
        r[3] = 0; g[3] = 0; b[3] = 0;
      }

      uint32_t indices = src[4] | (uint32_t(src[5]) << 8) | (uint32_t(src[6]) << 16) | (uint32_t(src[7]) << 24);
      for (int i = 0; i < 16; i++) {
        int idx = (indices >> (2 * i)) & 3;
        dst[i][0] = r[idx]; dst[i][1] = g[idx]; dst[i][2] = b[idx];
        dst[i][3] = (c0 > c1) ? 255u : ((idx == 3) ? 0u : 255u);
      }
    }

    void decodeBC4Alpha(uint8_t dst[16], const uint8_t src[8]) {
      uint8_t a0 = src[0], a1 = src[1];
      uint64_t indices = uint64_t(src[2]) | (uint64_t(src[3]) << 8)
                       | (uint64_t(src[4]) << 16) | (uint64_t(src[5]) << 24)
                       | (uint64_t(src[6]) << 32) | (uint64_t(src[7]) << 40);
      for (int i = 0; i < 16; i++) {
        int idx = (indices >> (3 * i)) & 7;
        if (a0 > a1) {
          static const uint8_t bc4[8] = {0,1,2,3,4,5,6,7};
          dst[i] = uint8_t((a0 * (7 - bc4[idx]) + a1 * bc4[idx]) / 7);
        } else {
          if (idx == 0) dst[i] = a0;
          else if (idx == 1) dst[i] = a1;
          else if (idx <= 5) dst[i] = uint8_t(((6 - idx) * a0 + (idx - 1) * a1) / 5);
          else dst[i] = 0;
        }
      }
    }

    void decodeBC4(uint8_t dst[16], const uint8_t src[8]) {
      decodeBC4Alpha(dst, src);
    }

    void decodeBC3(uint8_t dst[16][4], const uint8_t src[16]) {
      decodeBC1(dst, src);
      uint8_t alpha[16];
      decodeBC4Alpha(alpha, src + 8);
      for (int i = 0; i < 16; i++)
        dst[i][3] = alpha[i];
    }

    void decodeBC5(uint8_t dst[16][4], const uint8_t src[16]) {
      uint8_t r[16], g[16];
      decodeBC4(r, src);
      decodeBC4(g, src + 8);
      for (int i = 0; i < 16; i++) {
        dst[i][0] = r[i]; dst[i][1] = g[i];
        dst[i][2] = 0;    dst[i][3] = 255;
      }
    }

    void decodeBC7(uint8_t dst[16][4], const uint8_t src[16]) {
      uint8_t mode = src[0];
      for (int i = 0; i < 16; i++) {
        dst[i][0] = 128; dst[i][1] = 128;
        dst[i][2] = 128; dst[i][3] = 255;
      }
      if ((mode & 0x80) == 0) return;

      uint16_t r0 = ((src[1] >> 1) & 0x7F) << 1 | (src[2] >> 7);
      uint16_t r1 = (src[2] & 0x7F) << 1 | (src[3] >> 7);
      uint16_t g0 = ((src[3] >> 1) & 0x7F) << 1 | (src[4] >> 7);
      uint16_t g1 = (src[4] & 0x7F) << 1 | (src[5] >> 7);
      uint16_t b0 = ((src[5] >> 1) & 0x7F) << 1 | (src[6] >> 7);
      uint16_t b1 = (src[6] & 0x7F) << 1 | (src[7] >> 7);
      uint8_t a0 = src[8], a1 = src[9];

      uint64_t indices = 0;
      for (int j = 0; j < 6; j++)
        indices |= uint64_t(src[10 + j]) << (8 * j);

      for (int i = 0; i < 16; i++) {
        int idx = (indices >> (4 * i)) & 0xF;
        dst[i][0] = uint8_t(((r0 * (64 - idx) + r1 * idx) * 255) / (63 * 64));
        dst[i][1] = uint8_t(((g0 * (64 - idx) + g1 * idx) * 255) / (63 * 64));
        dst[i][2] = uint8_t(((b0 * (64 - idx) + b1 * idx) * 255) / (63 * 64));
        dst[i][3] = uint8_t(((a0 * (64 - idx) + a1 * idx) * 255) / (63 * 64));
      }
    }

    // --- ASTC block encoder (simplified, 1 partition, LDR) ---

    void astcSetBits(uint8_t* block, uint32_t& bitPos, uint32_t count, uint32_t value) {
      for (uint32_t i = 0; i < count; i++) {
        uint32_t byteIdx = bitPos >> 3;
        uint32_t bitIdx = bitPos & 7;
        if (value & (1u << i))
          block[byteIdx] |= (1u << bitIdx);
        else
          block[byteIdx] &= ~(1u << bitIdx);
        bitPos++;
      }
    }

    uint32_t astcBlockMode(uint32_t w, uint32_t h, uint32_t bits) {
      if (w <= 11 && h <= 5 && bits >= 2 && bits <= 5) {
        uint32_t b = w - 4;
        uint32_t c = h - 2;
        uint32_t d = bits - 2;
        return (0 << 0) | (0 << 1) | ((b & 7) << 2) | ((c & 3) << 5) | ((d & 3) << 7);
      }
      if (w == h) {
        uint32_t D = w - 2;
        uint32_t wb = bits - 1;
        return (0 << 0) | (1 << 1) | (3 << 2) | (3 << 4) | ((D & 7) << 6) | ((wb & 3) << 9);
      }
      return (0 << 0) | (1 << 1) | (3 << 2) | (3 << 4) | (4 << 6) | (1 << 9);
    }

    bool astcBlockDims(VkFormat fmt, int& bw, int& bh) {
      switch (fmt) {
        case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:   bw = 4;  bh = 4;  return true;
        case VK_FORMAT_ASTC_5x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x4_SRGB_BLOCK:   bw = 5;  bh = 4;  return true;
        case VK_FORMAT_ASTC_5x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x5_SRGB_BLOCK:   bw = 5;  bh = 5;  return true;
        case VK_FORMAT_ASTC_6x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x5_SRGB_BLOCK:   bw = 6;  bh = 5;  return true;
        case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x6_SRGB_BLOCK:   bw = 6;  bh = 6;  return true;
        case VK_FORMAT_ASTC_8x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x5_SRGB_BLOCK:   bw = 8;  bh = 5;  return true;
        case VK_FORMAT_ASTC_8x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x6_SRGB_BLOCK:   bw = 8;  bh = 6;  return true;
        case VK_FORMAT_ASTC_8x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x8_SRGB_BLOCK:   bw = 8;  bh = 8;  return true;
        case VK_FORMAT_ASTC_10x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x5_SRGB_BLOCK:  bw = 10; bh = 5;  return true;
        case VK_FORMAT_ASTC_10x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x6_SRGB_BLOCK:  bw = 10; bh = 6;  return true;
        case VK_FORMAT_ASTC_10x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x8_SRGB_BLOCK:  bw = 10; bh = 8;  return true;
        case VK_FORMAT_ASTC_10x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x10_SRGB_BLOCK: bw = 10; bh = 10; return true;
        case VK_FORMAT_ASTC_12x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x10_SRGB_BLOCK: bw = 12; bh = 10; return true;
        case VK_FORMAT_ASTC_12x12_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x12_SRGB_BLOCK: bw = 12; bh = 12; return true;
        default: bw = 0; bh = 0; return false;
      }
    }

    void encodeAstcBlock(uint8_t* dst, const uint8_t pixels[], int bw, int bh, bool hasAlpha) {
      memset(dst, 0, 16);

      int weightBits = (bw <= 4 && bh <= 4) ? 4 : (bw <= 5 && bh <= 5) ? 3 : 2;

      uint32_t bp = 0;

      uint32_t bm = astcBlockMode(bw, bh, weightBits);
      astcSetBits(dst, bp, 13, bm);

      astcSetBits(dst, bp, 2, 0);

      uint32_t cem = hasAlpha ? 8 : 6;
      astcSetBits(dst, bp, 4, cem);

      int pixelCount = bw * bh;
      uint8_t minR = 255, minG = 255, minB = 255, maxR = 0, maxG = 0, maxB = 0;
      uint8_t minA = 255, maxA = 0;

      for (int i = 0; i < pixelCount; i++) {
        uint8_t r = pixels[4*i+0], g = pixels[4*i+1], b = pixels[4*i+2], a = pixels[4*i+3];
        if (r < minR) minR = r; if (r > maxR) maxR = r;
        if (g < minG) minG = g; if (g > maxG) maxG = g;
        if (b < minB) minB = b; if (b > maxB) maxB = b;
        if (a < minA) minA = a; if (a > maxA) maxA = a;
      }

      int epBits = 5;
      if (!hasAlpha) {
        if (bw <= 4 && bh <= 4)       epBits = 6;
        else if (bw <= 5 && bh <= 5)  epBits = 5;
        else                           epBits = 4;
      } else {
        if (bw <= 4 && bh <= 4)       epBits = 5;
        else if (bw <= 5 && bh <= 5)  epBits = 4;
        else                           epBits = 4;
      }

      int epShift = 8 - epBits;
      uint32_t epR0 = minR >> epShift, epG0 = minG >> epShift, epB0 = minB >> epShift;
      uint32_t epR1 = maxR >> epShift, epG1 = maxG >> epShift, epB1 = maxB >> epShift;

      if (hasAlpha) {
        uint32_t epA0 = minA >> epShift, epA1 = maxA >> epShift;
        astcSetBits(dst, bp, epBits, epR0);
        astcSetBits(dst, bp, epBits, epG0);
        astcSetBits(dst, bp, epBits, epB0);
        astcSetBits(dst, bp, epBits, epR1);
        astcSetBits(dst, bp, epBits, epG1);
        astcSetBits(dst, bp, epBits, epB1);
        astcSetBits(dst, bp, epBits, epA0);
        astcSetBits(dst, bp, epBits, epA1);
      } else {
        astcSetBits(dst, bp, epBits, epR0);
        astcSetBits(dst, bp, epBits, epG0);
        astcSetBits(dst, bp, epBits, epB0);
        astcSetBits(dst, bp, epBits, epR1);
        astcSetBits(dst, bp, epBits, epG1);
        astcSetBits(dst, bp, epBits, epB1);
      }

      for (int ty = 0; ty < bh; ty++) {
        for (int tx = 0; tx < bw; tx++) {
          int morton = 0;
          for (int b = 0; b < 4; b++) {
            morton |= ((tx >> b) & 1) << (2 * b);
            morton |= ((ty >> b) & 1) << (2 * b + 1);
          }
          int i = morton;

          uint8_t r = pixels[4*i+0], g = pixels[4*i+1], b = pixels[4*i+2];
          uint8_t a = pixels[4*i+3];

          float w = 0.0f;
          if (maxR > minR) w = std::max(w, float(r - minR) / float(maxR - minR));
          if (maxG > minG) w = std::max(w, float(g - minG) / float(maxG - minG));
          if (maxB > minB) w = std::max(w, float(b - minB) / float(maxB - minB));

          if (hasAlpha && maxA > minA)
            w = std::max(w, float(a - minA) / float(maxA - minA));

          int weight = int(w * ((1 << weightBits) - 1) + 0.5f);
          if (weight < 0) weight = 0;
          if (weight >= (1 << weightBits)) weight = (1 << weightBits) - 1;

          astcSetBits(dst, bp, weightBits, weight);
        }
      }

      while (bp < 128)
        astcSetBits(dst, bp, 1, 0);
    }

  } // anonymous namespace

  void Vegas::transcodeImageData(
      void*                 dstData,
      const void*           srcData,
      VkFormat              srcFormat,
      VkFormat              dstFormat,
      uint32_t              width,
      uint32_t              height) {
    int srcBw = 4, srcBh = 4;
    int dstBw, dstBh;
    if (!astcBlockDims(dstFormat, dstBw, dstBh))
      return;

    int srcBlocksX = (int(width) + srcBw - 1) / srcBw;
    int srcBlocksY = (int(height) + srcBh - 1) / srcBh;
    int dstBlocksX = (int(width) + dstBw - 1) / dstBw;
    int dstBlocksY = (int(height) + dstBh - 1) / dstBh;

    int srcBlockSize = 16;
    if (srcFormat == VK_FORMAT_BC1_RGB_UNORM_BLOCK || srcFormat == VK_FORMAT_BC1_RGB_SRGB_BLOCK ||
        srcFormat == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || srcFormat == VK_FORMAT_BC1_RGBA_SRGB_BLOCK ||
        srcFormat == VK_FORMAT_BC4_UNORM_BLOCK || srcFormat == VK_FORMAT_BC4_SNORM_BLOCK) {
      srcBlockSize = 8;
    }

    int scratchTexels = srcBlocksX * srcBw * height;
    uint8_t* decodedPixels = new uint8_t[scratchTexels * 4];
    uint8_t blockPixels[16][4];
    bool hasAlpha = false;

    for (int by = 0; by < srcBlocksY; by++) {
      for (int bx = 0; bx < srcBlocksX; bx++) {
        const uint8_t* srcBlock = (const uint8_t*)srcData + (by * srcBlocksX + bx) * srcBlockSize;

        switch (srcFormat) {
          case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
          case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
          case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
          case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
            decodeBC1(blockPixels, srcBlock);
            break;
          case VK_FORMAT_BC3_UNORM_BLOCK:
          case VK_FORMAT_BC3_SRGB_BLOCK:
            decodeBC3(blockPixels, srcBlock);
            hasAlpha = true;
            break;
          case VK_FORMAT_BC4_UNORM_BLOCK:
          case VK_FORMAT_BC4_SNORM_BLOCK: {
            uint8_t* rp = (uint8_t*)blockPixels;
            memset(rp, 0, 16 * 4);
            decodeBC4(rp, srcBlock);
            break;
          }
          case VK_FORMAT_BC5_UNORM_BLOCK:
          case VK_FORMAT_BC5_SNORM_BLOCK:
            decodeBC5(blockPixels, srcBlock);
            break;
          case VK_FORMAT_BC7_UNORM_BLOCK:
          case VK_FORMAT_BC7_SRGB_BLOCK:
            decodeBC7(blockPixels, srcBlock);
            hasAlpha = true;
            break;
          default:
            delete[] decodedPixels;
            return;
        }

        for (uint32_t py = 0; py < srcBw && (by * srcBh + py) < height; py++) {
          for (uint32_t px = 0; px < srcBh && (bx * srcBw + px) < width; px++) {
            int gi = int((by * srcBh + py) * width + (bx * srcBw + px));
            int bi = int(py * srcBw + px);
            decodedPixels[gi*4+0] = blockPixels[bi][0];
            decodedPixels[gi*4+1] = blockPixels[bi][1];
            decodedPixels[gi*4+2] = blockPixels[bi][2];
            decodedPixels[gi*4+3] = blockPixels[bi][3];
          }
        }
      }
    }

    for (int by = 0; by < dstBlocksY; by++) {
      for (int bx = 0; bx < dstBlocksX; bx++) {
        uint8_t astcPixels[12 * 12 * 4];
        int idx = 0;
        for (int py = 0; py < dstBh; py++) {
          int sy = by * dstBh + py;
          if (sy >= int(height)) sy = int(height) - 1;
          for (int px = 0; px < dstBw; px++) {
            int sx = bx * dstBw + px;
            if (sx >= int(width)) sx = int(width) - 1;
            int si = sy * int(width) + sx;
            astcPixels[idx*4+0] = decodedPixels[si*4+0];
            astcPixels[idx*4+1] = decodedPixels[si*4+1];
            astcPixels[idx*4+2] = decodedPixels[si*4+2];
            astcPixels[idx*4+3] = decodedPixels[si*4+3];
            idx++;
          }
        }

        uint8_t astcBlock[16];
        encodeAstcBlock(astcBlock, astcPixels, dstBw, dstBh, hasAlpha);

        int dstIdx = by * dstBlocksX + bx;
        memcpy((uint8_t*)dstData + dstIdx * 16, astcBlock, 16);
      }
    }

    delete[] decodedPixels;
  }


  // ============================================================
  // Self-Aware Profile — auto-detect GPU, bake all thresholds
  // ============================================================

  void Vegas::initializeProfile(DxvkDevice* device) {
    if (s_initialized) return;

    if (device == nullptr || device->adapter() == nullptr) {
      s_initialized = true;
      return;
    }

    auto& props = device->adapter()->deviceProperties().core.properties;
    bool isAdreno = device->adapter()->isAdreno();

    if (!isAdreno) {
      // Fallback: check device name
      std::string dname(props.deviceName);
      for (auto& c : dname) c = std::tolower(static_cast<unsigned char>(c));
      isAdreno = (dname.find("adreno") != std::string::npos);
    }

    if (isAdreno) {
      s_enabled        = true;
      s_bindSkipEnabled = true;
      s_tier           = device->adapter()->getStarEnginePersona();
    } else {
      s_enabled        = false;
      s_bindSkipEnabled = false;
      s_tier           = 0;
    }

    // Bake draw thresholds based on GPU tier (D3D11 base)
    static constexpr uint32_t drawThresholdTable[] = { 600, 1200, 2000 };
    static constexpr uint32_t haaeThresholdTable[] = { 40,  65,   100 };

    uint32_t idx = (s_tier >= 1 && s_tier <= 3) ? s_tier - 1 : 0;
    s_drawThreshold = drawThresholdTable[idx];
    s_haaeThreshold = haaeThresholdTable[idx];

    s_initialized = true;
  }

  bool Vegas::isEnabled()           { return s_enabled; }
  bool Vegas::isBindSkipEnabled()   { return s_bindSkipEnabled; }
  uint32_t Vegas::getDrawThreshold() { return s_drawThreshold; }
  uint32_t Vegas::getHaaeThreshold() { return s_haaeThreshold; }
  uint32_t Vegas::getTier()         { return s_tier; }

  // ============================================================
  // Decision Helpers — all feature logic lives here
  // ============================================================

  bool Vegas::shouldFlush(uint32_t drawCount) {
    return s_enabled && drawCount >= s_drawThreshold;
  }

  bool Vegas::shouldSkipBind() {
    return s_enabled && s_bindSkipEnabled;
  }

  bool Vegas::shouldSubmitHaae(uint32_t& counter, uint32_t drawCalls) {
    counter += drawCalls;
    if (counter >= s_haaeThreshold) {
      counter = 0;
      return true;
    }
    return false;
  }

  bool Vegas::shouldUpscale(Tristate upscalerState, VkExtent3D src, VkExtent3D dst) {
    if (upscalerState == Tristate::False)
      return false;
    if (upscalerState == Tristate::True)
      return true;
    // Auto: only upscale when source is smaller than destination
    return src.width < dst.width;
  }

} // namespace dxvk
