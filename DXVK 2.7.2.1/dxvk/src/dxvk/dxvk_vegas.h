#pragma once

#include <cstdint>
#include <cstring>

#include "dxvk_adapter.h"

namespace dxvk {
  class Config; // fwd decl for Config-based overloads
  enum class Tristate : int32_t; // fwd decl for shouldUpscale()

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
   * \brief Per-context Vegas runtime state
   *
   * Instance stored in DxvkContext. The type lives here so all
   * feature decision logic is defined alongside the state it reads.
   */
  struct VegasProfile {
    bool           initialized           = false;
    bool           enabled               = false;
    VkPipeline     lastBoundVkPipeline    = VK_NULL_HANDLE;
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

    // ============================================================
    // Self-Aware Profile — All thresholds baked internally
    // ============================================================

    /// Initialize the self-aware profile. Detects GPU, tier,
    /// and bakes all thresholds. Idempotent (safe to call multiple times).
    static void initializeProfile(
            DxvkDevice*          device);

    /// True if Vegas optimizations are active (Adreno GPU detected)
    static bool isEnabled();

    /// True if pipeline bind-skip is active
    static bool isBindSkipEnabled();

    /// Bake-determined draw-call flush threshold
    static uint32_t getDrawThreshold();

    /// Bake-determined HAAE submission threshold
    static uint32_t getHaaeThreshold();

    /// Detected GPU tier (1=entry, 2=mid, 3=high)
    static uint32_t getTier();

    // ---- Decision Helpers (consolidated feature logic) ----

    /// Should the caller flush pending draws?
    static bool shouldFlush(
            uint32_t             drawCount);

    /// Should the caller skip binding descriptors?
    static bool shouldSkipBind();

    /// Should the caller submit HAAE early? Manages counter internally.
    /// \param [in,out] counter Caller-owned draw counter (incremented internally)
    /// \param [in]     drawCalls Number of draws in the current submission
    static bool shouldSubmitHaae(
            uint32_t&            counter,
            uint32_t             drawCalls);

    // ---- Legacy Profile (kept for compat, not user-facing) ----

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

    /// Self-contained overload — reads internal s_tier and modifies
    /// s_drawThreshold directly. Caller only supplies load + frameTime.
    static void tuneThreshold(
            float                load,
            float                frameTime);

    static bool shouldZeroInit(
            uint32_t             tier);

    // ---- Utility ----

    static void calculateAspectRatio(
            uint32_t             w,
            uint32_t             h,
            float&               outX,
            float&               outY);

    static uint64_t getSystemRamMB();

    // ---- HW Masking (baked, not user-tunable) ----

    /** Apply VRAM scaling at config-load time (self-aware, Config overload) */
    static void applyVramSwap(
            Config&              config);

    /** Apply GPU persona at config-load time (self-aware, Config overload) */
    static void applyGpuMask(
            Config&              config);

    /** Apply VRAM scaling at Vk-device-init time (tier from initializeProfile) */
    static void applyVramSwap(
            VkPhysicalDeviceMemoryProperties& props,
            uint32_t             tier);

    /** Apply GPU persona at Vk-device-init time (tier from initializeProfile) */
    static void applyGpuMask(
            VkPhysicalDeviceProperties&       props,
            uint32_t             persona);

    // ---- Frame Gen (baked) ----

    static bool needsFrameGen(
            float                frameTime,
            uint32_t             tier);

    // ---- FSR (user-facing only via Tristate config) ----

    static void calculateFsrConstants(
            VegasFsrConstants&   c,
            VkExtent3D           src,
            VkExtent3D           dst);

    /// Should FSR upscale be applied? Resolves Tristate against src/dst extents.
    static bool shouldUpscale(
            Tristate             upscalerState,
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

    // ---- BCn→ASTC Transcoder (baked auto) ----

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

  private:

    // Baked state — set once by initializeProfile(), never user-tunable
    static bool                s_initialized;
    static bool                s_enabled;
    static bool                s_bindSkipEnabled;
    static uint32_t            s_tier;
    static uint32_t            s_drawThreshold;
    static uint32_t            s_haaeThreshold;
  };

} // namespace dxvk
