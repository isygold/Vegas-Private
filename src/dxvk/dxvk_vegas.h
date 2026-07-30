#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <array>
#include <chrono>

#include "dxvk_adapter.h"

namespace dxvk {

  /**
   * \brief VEGAS Autonomous Governor state
   *
   * Self-contained state for the closed-loop adaptive governor.
   * Updated by UpdateFrameTiming / CalculateThreshold / EndOfFrameCleanup
   * called from the swapchain Present path.
   */
  struct VegasGovernorState {
    // Frame accumulators
    uint32_t frameDrawCount = 0;
    uint32_t previousFrameDrawCount = 0;
    uint32_t drawThreshold = 100;
    uint32_t targetFlushesPerFrame = 4;

    // Timing state
    float smoothFrameTimeMs = 0.0f;
    float ftRatio = 0.0f;

    // Self-calibrating cap state
    uint32_t rollingMaxDraws = 0;
    uint32_t rollingMinDraws = UINT32_MAX;
    float    rollingVarianceRatio = 0.0f;
    uint32_t dynamicMaxBatchCap = 2048;
    uint32_t floorMinimumCap = 64;

    // Rolling window data
    std::array<uint32_t, 120> drawHistoryWindow{};
    uint8_t windowIndex = 0;
    uint8_t windowScanCounter = 0;
  };
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
   * FSR upscaling support, and
   * governor-style adaptive threshold tuning.
   */
  // Forward declarations for DXVK types used in async FSR
  class DxvkFence;
  class DxvkDevice;

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

    /// Update frame timing with dynamic EMA, shader stutter guard,
    /// and closed-loop targetFlushesPerFrame tuning.
    /// Returns ftRatio (smoothFrameTimeMs / targetFrameTimeMs) for EndOfFrameCleanup.
    static float updateFrameTiming(
            float                gpuLoad,
            float                frameTime);

    /// Calculate draw threshold for the next frame using predicted draw count,
    /// self-calibrating dynamicMaxBatchCap, and variance guard.
    static void calculateThreshold();

    /// End-of-frame cleanup: rolling window update, self-calibrating cap,
    /// predictor update, frame counter reset. Reads ftRatio + gpuLoad from
    /// stored governor/HUD state.
    static void endOfFrameCleanup();

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

    // ---- Frame Gen (baked) ----

    /// Should framegen be enabled this frame?
    /// \returns true if performance has headroom for interpolation.
    /// Tier 1 always returns false (compute budget insufficient).
    static bool needsFrameGen(
            float                frameTime,
            uint32_t             tier);

    /// Dispatch 3-pass motion-compensated framegen.
    /// Generates interpolated frame between current and saved previous.
    /// Returns true when the Frame Generator has a valid VkDevice/VkQueue
    /// and can actually dispatch. Callers should check this before entering
    /// the FG evaluation path (analyzePerformance, tuneThreshold, needsFrameGen)
    /// to avoid wasting CPU cycles on logging and threshold tuning that
    /// cannot be acted upon.
    static bool isFrameGenReady();

    /// Dispatch 3-pass motion-compensated framegen.
    /// Generates interpolated frame between current and saved previous.
    /// \param [in] curImage  Current rendered frame (VK_IMAGE_LAYOUT_GENERAL)
    /// \param [in] prevImage Previous frame (VK_IMAGE_LAYOUT_GENERAL)
    /// \param [in] extent    Image dimensions
    /// \param [in] format    Image format (must be R8G8B8A8_UNORM)
    /// \returns true if dispatch completed successfully
    static bool framegenDispatch(
            VkImage              curImage,
            VkImage              prevImage,
            VkExtent3D           extent,
            VkFormat             format);

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

    // ---- FSR Upscale Dispatch (baked, fail-closed) ----

    /// Attempt FSR 1.0 EASU upscale dispatch.
    /// \param [in] srcImage Source image (render output, low-res)
    /// \param [in] dstImage Destination image (swapchain output, high-res)
    /// \param [in] srcExtent Source image extent
    /// \param [in] dstExtent Destination image extent
    /// \param [in] swapchainFormat VkFormat of the swapchain
    /// \param [in] fsrConsts Pre-computed FSR constants from calculateFsrConstants
    /// \returns true if the dispatch was successfully submitted and completed
    /// \note Fail-closed: returns false on any error. Never crashes the frame.
    /// Synchronous FSR dispatch (legacy — blocks CPU with vkWaitForFences).
    /// Preferred: use fsrUpscaleAsync + fsrTryBlitResult for zero CPU blocking.
    static bool fsrUpscale(
            VkImage              srcImage,
            VkImage              dstImage,
            VkExtent3D           srcExtent,
            VkExtent3D           dstExtent,
            VkFormat             swapchainFormat,
            VegasFsrConstants&   fsrConsts);

    /// Async FSR dispatch using DxvkFence (timeline semaphore, leegao).
    /// Submits EASU compute to s_fsrInterImage without blocking.
    /// GPU completion is tracked via s_fsrFence for non-blocking check.
    /// Caller should call fsrTryBlitResult BEFORE this on each frame
    /// to blit the previous frame's completed result.
    /// \returns true if compute was successfully submitted.
    static bool fsrUpscaleAsync(
            VkImage              srcImage,
            VkExtent3D           srcExtent,
            VkExtent3D           dstExtent,
            VkFormat             swapchainFormat,
            VegasFsrConstants&   fsrConsts);

    /// Non-blocking blit of the previous frame's completed FSR result.
    /// Checks s_fsrFence (timeline semaphore) — if the previous async
    /// FSR compute has finished, blits s_fsrInterImage → dstImage
    /// with a fast synchronous vkCmdBlitImage.
    /// \returns true if a completed FSR frame was blitted to dst.
    static bool fsrTryBlitResult(
            VkImage              dstImage,
            VkExtent3D           dstExtent);

    /// Drain any in-flight async FSR compute (blocks until GPU completes).
    /// Safe to call before destroying the intermediate image (e.g. on
    /// swapchain resize).  Idempotent — safe to call even if no FSR
    /// is in flight.
    static void fsrDrain();

    /// Retrieve framegen output VkImage (interpolated intermediate frame).
    /// Note: framegenDispatch blits the output to curImage internally;
    /// this getter exists for debug/inspection only.
    static uint64_t framegenOutputImage();

  public:

    // Baked state — set once by initializeProfile(), never user-tunable
    static bool                s_initialized;
    static bool                s_enabled;
    static bool                s_bindSkipEnabled;
    static bool                s_useFastPath;
    static uint32_t            s_tier;
    static uint32_t            s_drawThreshold;
    static uint32_t            s_haaeThreshold;

    // Vulkan state (opaque handles, defined in .cpp with full DXVK includes)
    static void*               s_device;          ///< VkDevice
    static uint64_t            s_physicalDevice;  ///< VkPhysicalDevice (for mem type lookup)
    static uint64_t            s_vkQueue;         ///< VkQueue
    static uint32_t            s_queueFamily;
    // FSR pipeline cache (opaque Vulkan handles)
    static uint64_t            s_fsrPipeline;       ///< VkPipeline
    static uint64_t            s_fsrPipelineLayout; ///< VkPipelineLayout
    static uint64_t            s_fsrDescSetLayout;  ///< VkDescriptorSetLayout
    static uint64_t            s_fsrDescPool;       ///< VkDescriptorPool
    static bool                s_fsrInitialized;

    // Intermediate FSR target — avoids VK_IMAGE_USAGE_STORAGE_BIT on swapchain
    static uint64_t            s_fsrInterImage;     ///< VkImage
    static uint64_t            s_fsrInterMemory;    ///< VkDeviceMemory
    static uint32_t            s_fsrInterW;         ///< current width
    static uint32_t            s_fsrInterH;         ///< current height

    // ---- Framegen resources ----
    static uint64_t            s_fgPipeline[3];       ///< VkPipeline (motion, median, warp)
    static uint64_t            s_fgPipelineLayout;    ///< VkPipelineLayout
    static uint64_t            s_fgDescSetLayout;     ///< VkDescriptorSetLayout
    static uint64_t            s_fgDescPool;          ///< VkDescriptorPool
    static bool                s_fgInitialized;

    // Framegen intermediate images
    static bool                s_fgPrevValid;         ///< true after first frame saved to prev
    static uint64_t            s_fgPrevImage;         ///< VkImage (saved previous frame)
    static uint64_t            s_fgPrevMemory;        ///< VkDeviceMemory
    static uint32_t            s_fgPrevW;             ///< current width
    static uint32_t            s_fgPrevH;             ///< current height
    static uint64_t            s_fgMotionImage;       ///< VkImage (raw motion, R32G32_SFLOAT)
    static uint64_t            s_fgMotionMemory;      ///< VkDeviceMemory
    static uint64_t            s_fgMotionFiltered;    ///< VkImage (filtered motion, R32G32_SFLOAT)
    static uint64_t            s_fgMotionFMemory;     ///< VkDeviceMemory
    static uint64_t            s_fgOutputImage;       ///< VkImage (framegen output)
    static uint64_t            s_fgOutputMemory;      ///< VkDeviceMemory
    static uint32_t            s_fgMotionW;           ///< motion buffer width (blocks)
    static uint32_t            s_fgMotionH;           ///< motion buffer height (blocks)

    // ---- VegasHud metrics (updated by pushMetrics()) ----
    static constexpr uint32_t  FT_HISTORY_SIZE = 60;
    static float               s_ftHistory[FT_HISTORY_SIZE];
    static uint32_t            s_ftHead;
    static float               s_lastGpuLoad;
    static VegasPerformanceState s_lastPerfState;
    static float               s_lastFrameTime;
    static bool                s_fsrActive;

    // ---- Draw count histogram (per-frame count, circular history, CSV dump) ----
    static constexpr uint32_t  DRAW_HISTORY_SIZE = 60;
    static uint32_t            s_drawHistory[DRAW_HISTORY_SIZE];
    static uint32_t            s_drawHead;
    static uint32_t            s_frameDrawCount;
    static uint32_t            s_dumpCounter;
    static bool                s_profileActive;
    static uint64_t            s_profileFrame;

    // ---- Autonomous governor state (VegasGovernorState) ----
    static VegasGovernorState  s_gov;
    static void recordDrawCall();
    static void dumpDrawCsv();

    // DxvkDevice stored for DxvkFence creation (set by initializeProfile)
    static DxvkDevice*         s_dxvkDevice;

    // ---- Async FSR (DxvkFence / timeline semaphore) ----
    static void*               s_fsrFence;        ///< DxvkFence* (timeline semaphore wrapper)
    static uint64_t            s_fsrNextValue;    ///< next timeline signal value
    static bool                s_fsrInFlight;     ///< FSR compute submitted, awaiting GPU completion
    static uint32_t            s_fsrResultW;      ///< completed result width
    static uint32_t            s_fsrResultH;      ///< completed result height
    static uint64_t            s_fsrLastSrcView;  ///< VkImageView from last async submit (destroy on fence signal)
    static uint64_t            s_fsrInterView;    ///< VkImageView (persistent, intermediate image — updated by ensureFsrIntermediate)
    static uint64_t            s_fsrAsyncCmdPool; ///< VkCommandPool (persistent, for async FSR submits)
    static uint64_t            s_fsrAsyncCmdBuf;  ///< VkCommandBuffer (persistent, for async FSR submits)

    static bool                s_fgActive;

    // ---- Session report state ----
    static bool                s_sessionActive;     ///< true between beginSession/endSession
    static bool                s_sessionCrashed;    ///< true if marker found at start (prior crash)
    static uint64_t            s_sessionFrames;     ///< total frames this session
    static uint32_t            s_fpsHistogram[25];  ///< 25-bin FPS histogram (0-120+, 5 FPS/bin)
    static double              s_minFps;            ///< minimum instant FPS this session
    static int64_t             s_sessionStartNs;    ///< steady_clock start in ns
    static int64_t             s_lastPresentNs;     ///< last Present timestamp in ns
    static std::string         s_markerPath;        ///< path to crash marker file
    static std::string         s_reportPath;        ///< path to report.json
    static std::string         s_issuePath;         ///< path to issue.md
    static std::string         s_gameName;          ///< sanitized game exe name

    /// Ensure the intermediate FSR image (s_fsrInterImage) exists and is
    /// large enough for the given extent. Recreates if dimensions changed.
    /// \returns true on success (image is ready for EASU dispatch).
    static bool ensureFsrIntermediate(
            VkDevice             device,
            VkExtent3D           extent);

    // ---- Session Report (auto crash reports, no manual file placement) ----

    /// Begin a session: write crash marker, detect prior crash.
    /// Auto-called from DxvkDevice constructor.
    static void beginSession();

    /// End a session: write report.json + issue.md, clean up marker.
    /// Auto-called from DxvkDevice destructor.
    static void endSession();

    /// Called once per Present: counts frames and tracks FPS histogram.
    /// Auto-called from device presentImage.
    static void onPresent();

    /// Sanitize an exe name to a safe filename fragment (no .exe, alnum only).
    static std::string sanitizeGameName(const std::string& exeName);

    /// Build marker/report/issue file paths from the executable name.
    static void buildSessionPaths();

    /// Push frame-timing metrics for HUD consumption.
    static void pushMetrics(
            float                gpuLoad,
            float                frameTime,
            VegasPerformanceState state,
            bool                 fsrActive,
            bool                 fgActive);

    /// Read frame time from circular history (relative index 0 = most recent).
    static float getHistoryFt(uint32_t idx);
    static uint32_t getHistoryFtCount();
    static float getLastGpuLoad();
    static VegasPerformanceState getLastPerfState();
    static float getLastFrameTime();
    static bool isFsrActive();
    static bool isFgActive();
  };

} // namespace dxvk
