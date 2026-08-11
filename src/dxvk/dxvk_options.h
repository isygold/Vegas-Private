#pragma once

#include "../util/config/config.h"

#include "../vulkan/vulkan_loader.h"

namespace dxvk {

  struct DxvkOptions {
    DxvkOptions() { }
    DxvkOptions(const Config& config);

    /// Enable debug utils
    bool enableDebugUtils = false;
	bool enableAsync;

    /// Enable memory defragmentation
    Tristate enableMemoryDefrag = Tristate::Auto;

    /// Number of compiler threads
    /// when using the state cache
    int32_t numCompilerThreads = 0;

    /// Enable graphics pipeline library
    Tristate enableGraphicsPipelineLibrary = Tristate::Auto;

    /// Enable descriptor buffer
    Tristate enableDescriptorBuffer = Tristate::Auto;

    /// Enables pipeline lifetime tracking
    Tristate trackPipelineLifetime = Tristate::Auto;

    /// Shader-related options
    Tristate useRawSsbo = Tristate::Auto;

    /// HUD elements
    std::string hud;

    /// Forces swap chain into MAILBOX (if true)
    /// or FIFO_RELAXED (if false) present mode
    Tristate tearFree = Tristate::Auto;

    /// Enables latency sleep
    Tristate latencySleep = Tristate::Auto;

    /// Latency tolerance, in microseconds
    int32_t latencyTolerance = 0u;

    /// Disable VK_NV_low_latency2. This extension
    /// appears to be all sorts of broken on 32-bit.
    Tristate disableNvLowLatency2 = Tristate::Auto;

    // Hides integrated GPUs if dedicated GPUs are
    // present. May be necessary for some games that
    // incorrectly assume monitor layouts.
    bool hideIntegratedGraphics = false;

    /// Clears all mapped memory to zero.
    bool zeroMappedMemory = false;

    /// Allows full-screen exclusive mode on Windows
    bool allowFse = false;

    /// Whether to enable tiler optimizations
    Tristate tilerMode = Tristate::Auto;

    /// Overrides memory budget for DXVK
    VkDeviceSize maxMemoryBudget = 0u;

    /// Whether to use custom sin/cos approximation
    Tristate lowerSinCos = Tristate::Auto;

    /// Device name
    std::string deviceFilter;

    // --- VEGAS: ADRENO OPTIMIZATION OPTIONS ---
    /// Master switch: enables all Vegas Adreno optimizations.
    /// Auto = enable on Adreno, True = force-on, False = force-off.
    Tristate enableStarProfile = Tristate::Auto;

    /// Enables FSR 1.0 spatial upscaler (Auto/True/False)
    /// Only effective when enableStarProfile is not False.
    Tristate vegasEnableUpscaler = Tristate::Auto;

    /// Framegen toggle (Auto/True/False).
    /// True = force-enable regardless of tier (Tier-1 testing).
    /// False = disable entirely (2.4.1 only honored True — False fell
    ///         through to the heuristic; this branch fixes that).
    /// Auto = tier headroom heuristic (Tier 2 ≤29ms, Tier 3 ≤33ms frame time).
    Tristate vegasEnableFramegen = Tristate::Auto;

    /// Override GPU tier (0 = auto-detect, 1 = entry, 2 = mid, 3 = high).
    /// Forces the tier detected by the GPU-name classifier to this value.
    /// Useful for misclassified Adreno GPUs or manual tuning.
    int32_t vegasForceTier = 0;

    /// Enables per-draw profiling CSV output (vegas_<game>_drawcount.csv).
    /// Equivalent to VEGAS_PROFILE_DRAWS=1.
    bool vegasProfileDraws = false;

    /// Reserved for framegen-stats telemetry. Parsed for dxvk.conf
    /// compatibility on this branch; the FG stats series is not ported here.
    bool vegasTelemetry = false;

    /// Test-only escape hatch: force BCn transcode even when the driver
    /// natively supports the format. Equivalent to VEGAS_FORCE_TRANSCODE=1.
    /// Never bypasses the isEnabled / s_device / s_tcAvailable guards.
    bool vegasForceTranscode = false;
  };

}
