#pragma once

#include <cstdint>
#include <cstring>

#include "dxvk_adapter.h"

namespace dxvk {

  /**
   * \brief Vegas performance state enum
   */
  enum class VegasPerformanceState : uint32_t {
    Normal       = 0,
    Lagging      = 1,
    Stuttering   = 2,
    Overheating  = 3,
  };

  /**
   * \brief Vegas FSR constants
   */
  struct VegasFsrConstants {
    float info[4];
  };

  /**
   * \brief Vegas — Star Engine next-gen optimization system
   *
   * Provides Adreno-optimized GPU profiling, dynamic VRAM/GPU masking,
   * BCn→ASTC texture transcoding, FSR upscaling support, and
   * governor-style adaptive threshold tuning.
   */
  class Vegas {

  public:

    // ---- Profile & Threshold ----

    static void initializeProfile(
            uint32_t&            threshold,
            bool&                enabled,
            bool&                bindSkip,
            uint32_t&            tier,
            DxvkDevice*          device);

    static void initializeProfile(
            uint32_t&            threshold,
            bool&                enabled,
            bool&                bindSkip,
            uint32_t&            tier,
            DxvkDevice*          device,
            bool                 isD3D9);

    static void tuneThreshold(
            uint32_t&            threshold,
            float                load,
            float                frameTime,
            uint32_t             tier);

    static bool shouldZeroInit(
            uint32_t             tier);

    // ---- Utility ----

    static void calculateAspectRatio(
            uint32_t             w,
            uint32_t             h,
            float&               outX,
            float&               outY);

    static uint64_t getSystemRamMB();

    // ---- HW Masking ----

    static void applyVramSwap(
            VkPhysicalDeviceMemoryProperties& props,
            uint32_t             tier);

    static void applyGpuMask(
            VkPhysicalDeviceProperties&       props,
            uint32_t             persona);

    // ---- Frame Gen ----

    static bool needsFrameGen(
            float                frameTime,
            uint32_t             tier);

    // ---- FSR ----

    static void calculateFsrConstants(
            VegasFsrConstants&   c,
            VkExtent3D           src,
            VkExtent3D           dst);

    // ---- Performance Analysis ----

    static VegasPerformanceState analyzePerformance(
            float                load,
            float                frameTime,
            float                targetFrameTime);

    static uint32_t getGraphColor(
            VegasPerformanceState state);

    static const char* getStatusString(
            VegasPerformanceState state);

    // ---- BCn→ASTC Transcoder ----

    static bool formatIsBcn(
            VkFormat             format);

    static VkFormat getAstcFormat(
            VkFormat             bcnFormat);

    static VkFormat shouldTranscodeFormat(
            VkFormat             originalFormat,
            VkImageUsageFlags    usage,
            VkExtent3D           extent,
            const Rc<DxvkAdapter>& adapter);

    static void transcodeImageData(
            void*                dstData,
            const void*          srcData,
            VkFormat             srcFormat,
            VkFormat             dstFormat,
            uint32_t             width,
            uint32_t             height);

  };

} // namespace dxvk
