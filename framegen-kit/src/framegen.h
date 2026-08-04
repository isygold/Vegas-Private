#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// VEGAS Frame Generation — standalone module
//
// Extracted from the VEGAS DXVK fork (release-v2.4.1).  This module provides
// the 3-pass motion-compensated frame generator (motion estimation → median
// filter → warp+blend) as a self-contained Vulkan compute pipeline.
//
// INTEGRATION CONTRACT
// ====================
// The host project must provide the following before calling any Framegen
// API:
//
//   1. A VkDevice, VkQueue, queue family index, and VkPhysicalDevice.
//      Call Framegen::init(device, queue, queueFamily, physicalDevice).
//   2. A Tristate framegen toggle (Auto / True / False).  Call
//      Framegen::setConfig(toggle) each frame or whenever the config changes.
//      Tristate mirrors dxvk_options.h's Tristate enum (Auto/True/False).
//   3. A smoothed frame-time value (ms) for the adaptive blend policy.
//      Call Framegen::setSmoothFrameTimeMs(frameTimeMs) each frame.
//   4. DXVK utilities: Logger (debug/warn), env::getEnvVar, str::format.
//      These are DXVK-internal; the kit ships a stub logger for testing.
//
// The module owns its own Vulkan function table (FgVulkanFuncs) and loads
// the required functions from the host device on first dispatch.
//
// LICENSE: zlib/libpng — see LICENSE file.
// ---------------------------------------------------------------------------

namespace dxvk {

  // Minimal Tristate matching dxvk_options.h — host should provide the real
  // one; this local copy avoids a hard dependency for kit consumers.
  enum class Tristate : int32_t {
    Auto = 0,
    True = 1,
    False = 2
  };

  class Framegen {
  public:
    // ------------------------------------------------------------------
    // Host integration — call once at device init
    // ------------------------------------------------------------------
    static void init(
            VkDevice           device,
            VkQueue            queue,
            uint32_t           queueFamily,
            VkPhysicalDevice   physicalDevice);

    // ------------------------------------------------------------------
    // Per-frame config — call each frame (or when config changes)
    // ------------------------------------------------------------------
    static void setConfig(Tristate toggle);
    static void setSmoothFrameTimeMs(float ms);

    // ------------------------------------------------------------------
    // Framegen eligibility (called from swapchain Present path)
    // ------------------------------------------------------------------
    // Returns true when the GPU has headroom for interpolation.
    // Tier 1 (Adreno 610/619) is excluded by design — compute budget.
    // Auto: tier+headroom gate.  True: force regardless of tier.
    // False: disable entirely (handled at isFrameGenReady).
    static bool needsFrameGen(float frameTimeMs, uint32_t tier);

    // Returns true when the Frame Generator has a valid VkDevice/VkQueue.
    // Also returns false when vegas.enableFramegen = False (user toggle).
    static bool isFrameGenReady();

    // ------------------------------------------------------------------
    // Dispatch the 3-pass framegen pipeline
    // ------------------------------------------------------------------
    // curImage   — current rendered frame (VK_IMAGE_LAYOUT_GENERAL)
    // prevImage  — previous frame (VK_IMAGE_LAYOUT_GENERAL; may be
    //              VK_NULL_HANDLE on first call — saves curImage and
    //              returns false)
    // extent     — image dimensions (swapchain resolution)
    // format     — must be R8G8B8A8_UNORM or B8G8R8A8_UNORM
    // hudRectValid — true when hudRect holds a valid HUD graph rect
    // hudRect    — [x0, y0, x1, y1] in WSI pixels; HUD region is
    //              exempted from framegen (avoids warping the graph)
    // Returns true when dispatch completed successfully.
    static bool dispatch(
            VkImage              curImage,
            VkImage              prevImage,
            VkExtent3D           extent,
            VkFormat             format,
            bool                 hudRectValid = false,
      const float*               hudRect      = nullptr);

    // ------------------------------------------------------------------
    // Get the interpolated output image (for debug/inspection only;
    // dispatch() blits the result to curImage internally)
    // ------------------------------------------------------------------
    static uint64_t outputImage();

    // ------------------------------------------------------------------
    // Framegen activity flag (for HUD metrics)
    // ------------------------------------------------------------------
    static bool isActive();

    // ------------------------------------------------------------------
    // Drain any in-flight async FG work (blocks until GPU completes).
    // Safe to call before swapchain resize.  Idempotent.
    // ------------------------------------------------------------------
    static void drain();

  private:
    // ---- Framegen statics ----
    static uint64_t            s_fgPipeline[3];       // VkPipeline (motion, median, warp)
    static uint64_t            s_fgPipelineLayout;    // VkPipelineLayout
    static uint64_t            s_fgDescSetLayout;     // VkDescriptorSetLayout
    static uint64_t            s_fgDescPool;          // VkDescriptorPool
    static bool                s_fgInitialized;

    // Telemetry (gated by env vegas_telemetry=1, default OFF)
    static uint64_t            s_fgStatsBuffer;       // VkBuffer (16B = 4 words)
    static uint64_t            s_fgStatsMemory;       // VkDeviceMemory
    static void*               s_fgStatsMapping;      // persistent map
    static uint64_t            s_fgStatsLayout;       // VkDescriptorSetLayout
    static uint64_t            s_fgStatsPool;         // VkDescriptorPool
    static uint64_t            s_fgStatsSet;          // VkDescriptorSet
    static bool                s_fgStatsEnabled;
    static uint32_t            s_fgStatsFrames;
    static uint32_t            s_fgStatsCorrupt;

    // Pending-drain list for the fence-timeout path
    struct FgPendingResources {
      VkFence        fence     = VK_NULL_HANDLE;
      VkCommandPool  pool      = VK_NULL_HANDLE;
      VkCommandBuffer cmdBuf   = VK_NULL_HANDLE;
      VkImageView    views[6]  = {};
      uint32_t       viewCount = 0;
    };
    static FgPendingResources s_fgPending[4];
    static uint32_t           s_fgPendingCount;

    // Adaptive blend watchdog
    static uint32_t           s_fgSlowCount;
    static constexpr uint32_t FG_SKIP_WINDOW = 60;
    static uint32_t           s_fgSkipFrames;

    // Zero-pad helper for stat logs
    static std::string fmt03(uint32_t v);

    // Intermediate images
    static bool     s_fgPrevValid;
    static uint64_t s_fgPrevImage;
    static uint64_t s_fgPrevMemory;
    static uint32_t s_fgPrevW;
    static uint32_t s_fgPrevH;
    static VkFormat s_fgFormat;
    static uint64_t s_fgMotionImage;
    static uint64_t s_fgMotionMemory;
    static uint64_t s_fgMotionFiltered;
    static uint64_t s_fgMotionFMemory;
    static uint64_t s_fgMotionPrevImage;
    static uint64_t s_fgMotionPrevMemory;
    static bool     s_fgMotionPrevValid;
    static uint64_t s_fgOutputImage;
    static uint64_t s_fgOutputMemory;
    static uint32_t s_fgMotionW;
    static uint32_t s_fgMotionH;

    // Host-provided state (set via init() / setConfig() / setSmoothFrameTimeMs())
    static VkDevice           s_device;
    static VkQueue            s_queue;
    static uint32_t           s_queueFamily;
    static VkPhysicalDevice   s_physicalDevice;
    static Tristate           s_toggle;          // user config
    static float              s_smoothFrameTimeMs; // governor EMA proxy

    // Vulkan function table (FG subset only)
    struct FgVulkanFuncs {
      bool                    loaded = false;
      // Device functions
      PFN_vkCreateShaderModule     vkCreateShaderModule     = nullptr;
      PFN_vkDestroyShaderModule    vkDestroyShaderModule    = nullptr;
      PFN_vkCreatePipelineLayout   vkCreatePipelineLayout   = nullptr;
      PFN_vkDestroyPipelineLayout  vkDestroyPipelineLayout  = nullptr;
      PFN_vkCreateComputePipelines vkCreateComputePipelines = nullptr;
      PFN_vkDestroyPipeline        vkDestroyPipeline        = nullptr;
      PFN_vkCreateDescriptorSetLayout   vkCreateDescriptorSetLayout   = nullptr;
      PFN_vkDestroyDescriptorSetLayout  vkDestroyDescriptorSetLayout  = nullptr;
      PFN_vkCreateDescriptorPool        vkCreateDescriptorPool        = nullptr;
      PFN_vkDestroyDescriptorPool       vkDestroyDescriptorPool       = nullptr;
      PFN_vkResetDescriptorPool         vkResetDescriptorPool         = nullptr;
      PFN_vkAllocateDescriptorSets      vkAllocateDescriptorSets      = nullptr;
      PFN_vkUpdateDescriptorSets        vkUpdateDescriptorSets        = nullptr;
      PFN_vkCreateImageView        vkCreateImageView        = nullptr;
      PFN_vkDestroyImageView       vkDestroyImageView       = nullptr;
      PFN_vkCreateImage            vkCreateImage            = nullptr;
      PFN_vkDestroyImage           vkDestroyImage           = nullptr;
      PFN_vkAllocateMemory         vkAllocateMemory         = nullptr;
      PFN_vkFreeMemory             vkFreeMemory             = nullptr;
      PFN_vkMapMemory              vkMapMemory              = nullptr;
      PFN_vkUnmapMemory            vkUnmapMemory            = nullptr;
      PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = nullptr;
      PFN_vkBindImageMemory        vkBindImageMemory        = nullptr;
      PFN_vkCreateFence            vkCreateFence            = nullptr;
      PFN_vkDestroyFence           vkDestroyFence           = nullptr;
      PFN_vkCreateCommandPool      vkCreateCommandPool      = nullptr;
      PFN_vkDestroyCommandPool     vkDestroyCommandPool     = nullptr;
      PFN_vkAllocateCommandBuffers  vkAllocateCommandBuffers  = nullptr;
      PFN_vkFreeCommandBuffers      vkFreeCommandBuffers      = nullptr;
      PFN_vkBeginCommandBuffer     vkBeginCommandBuffer     = nullptr;
      PFN_vkEndCommandBuffer       vkEndCommandBuffer       = nullptr;
      PFN_vkQueueSubmit            vkQueueSubmit            = nullptr;
      PFN_vkWaitForFences          vkWaitForFences          = nullptr;
      PFN_vkGetFenceStatus         vkGetFenceStatus         = nullptr;
      PFN_vkCmdPipelineBarrier     vkCmdPipelineBarrier     = nullptr;
      PFN_vkCmdDispatch            vkCmdDispatch            = nullptr;
      PFN_vkCmdCopyImage           vkCmdCopyImage           = nullptr;
      PFN_vkCmdBlitImage           vkCmdBlitImage           = nullptr;
      PFN_vkCmdBindPipeline        vkCmdBindPipeline        = nullptr;
      PFN_vkCmdBindDescriptorSets  vkCmdBindDescriptorSets  = nullptr;
      PFN_vkCmdPushConstants       vkCmdPushConstants       = nullptr;
      PFN_vkCmdFillBuffer          vkCmdFillBuffer          = nullptr;
      // Physical-device function
      PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
      // Loader flag
      bool loaded = false;
    };
    static FgVulkanFuncs s_fgVk;

    // Internal helpers
    static bool fgLoadVulkanFuncs(VkDevice device);
    static bool initFgPipeline(VkDevice device);
    static bool ensureFgIntermediateImages(VkDevice device, uint32_t w, uint32_t h, VkFormat format);
    static void fgDestroyPendingEntry(VkDevice device, uint32_t idx);
    static void fgDrainPending(VkDevice device);
    static void fgQueuePending(VkDevice device, VkFence fence,
                               VkCommandPool pool, VkCommandBuffer cmdBuf,
                               VkImageView views[], uint32_t viewCount);
  };

} // namespace dxvk
