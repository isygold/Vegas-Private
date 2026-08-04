#pragma once

#include "../util/config/config.h"
#include "dxvk_include.h"

namespace dxvk {

  struct DxvkOptions {
    DxvkOptions() { }
    DxvkOptions(const Config& config);

    /// Enable debug utils
    bool enableDebugUtils;

    /// Enable state cache
    bool enableStateCache;

    /// Number of compiler threads
    /// when using the state cache
    int32_t numCompilerThreads;

    /// Enable graphics pipeline library
    Tristate enableGraphicsPipelineLibrary;

    /// Enables pipeline lifetime tracking
    Tristate trackPipelineLifetime;

    // Enable async pipelines
    bool enableAsync;
    // Enable state cache with gpl and fixes for async
    bool gplAsyncCache;

    /// Shader-related options
    Tristate useRawSsbo;

    /// HUD elements
    std::string hud;

    /// Forces swap chain into MAILBOX (if true)
    /// or FIFO_RELAXED (if false) present mode
    Tristate tearFree;

    // Hides integrated GPUs if dedicated GPUs are
    // present. May be necessary for some games that
    // incorrectly assume monitor layouts.
    bool hideIntegratedGraphics;

    // Device name
    std::string deviceFilter;

    /// Enable Star/Vegas optimization profile (Auto = detect Adreno)
    Tristate enableStarProfile = Tristate::Auto;

    /// Force GPU tier (0 = auto, 1-3 = manual override)
    int32_t vegasForceTier = 0;

    /// Frame generation toggle (Auto = tier + headroom gate,
    /// True = force enable regardless of tier, False = disable)
    Tristate vegasEnableFramegen = Tristate::Auto;

  };

}
