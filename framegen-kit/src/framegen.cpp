// ---------------------------------------------------------------------------
// VEGAS Frame Generation — standalone implementation
//
// Extracted from dxvk_vegas.cpp (release-v2.4.1, commit c07a9c8).
// Framegen-only: no governor, no FSR, no HUD, no GPU persona.
//
// External dependencies (provided by host):
//   - Vulkan device/queue/physicalDevice via Framegen::init()
//   - Tristate toggle via Framegen::setConfig()
//   - Smoothed frame-time via Framegen::setSmoothFrameTimeMs()
//   - Logger (debug/warn), env::getEnvVar, str::format
// ---------------------------------------------------------------------------

#include "framegen.h"
#include <cstring>
#include <thread>

// Stub Logger for kit consumers who don't link DXVK.
// Replace with real Logger in production builds.
namespace Logger {
  static void debug(const std::string&) {}
  static void warn(const std::string&) {}
}

// Stub env for kit consumers.
namespace env {
  static std::string getEnvVar(const char*) { return ""; }
}

// Stub str for kit consumers.
namespace str {
  template<typename... Args>
  static std::string format(const char*, Args...) { return ""; }
  // Explicit overload for the one str::format call used in framegen:
  static std::string format(const char* fmt) { return fmt ? fmt : ""; }
}

namespace dxvk {

// ===========================================================================
// Host-provided state (set via init() / setConfig() / setSmoothFrameTimeMs())
// ===========================================================================

VkDevice           Framegen::s_device         = VK_NULL_HANDLE;
VkQueue            Framegen::s_queue          = VK_NULL_HANDLE;
uint32_t           Framegen::s_queueFamily    = 0;
VkPhysicalDevice   Framegen::s_physicalDevice = VK_NULL_HANDLE;
Tristate           Framegen::s_toggle         = Tristate::Auto;
float              Framegen::s_smoothFrameTimeMs = 0.0f;

// ===========================================================================
// Framegen statics
// ===========================================================================

uint64_t Framegen::s_fgPipeline[3]     = {0, 0, 0};
uint64_t Framegen::s_fgPipelineLayout  = 0;
uint64_t Framegen::s_fgDescSetLayout   = 0;
uint64_t Framegen::s_fgDescPool        = 0;
bool     Framegen::s_fgInitialized     = false;

uint64_t Framegen::s_fgStatsBuffer   = 0;
uint64_t Framegen::s_fgStatsMemory   = 0;
void*    Framegen::s_fgStatsMapping  = nullptr;
uint64_t Framegen::s_fgStatsLayout   = 0;
uint64_t Framegen::s_fgStatsPool     = 0;
uint64_t Framegen::s_fgStatsSet      = 0;
bool     Framegen::s_fgStatsEnabled  = false;
uint32_t Framegen::s_fgStatsFrames   = 0;
uint32_t Framegen::s_fgStatsCorrupt  = 0;

Framegen::FgPendingResources Framegen::s_fgPending[4];
uint32_t Framegen::s_fgPendingCount = 0;

uint32_t Framegen::s_fgSlowCount    = 0;
uint32_t Framegen::s_fgSkipFrames   = 0;

bool     Framegen::s_fgPrevValid       = false;
uint64_t Framegen::s_fgPrevImage       = 0;
uint64_t Framegen::s_fgPrevMemory      = 0;
uint32_t Framegen::s_fgPrevW           = 0;
uint32_t Framegen::s_fgPrevH           = 0;
VkFormat Framegen::s_fgFormat          = VK_FORMAT_UNDEFINED;
uint64_t Framegen::s_fgMotionImage     = 0;
uint64_t Framegen::s_fgMotionMemory    = 0;
uint64_t Framegen::s_fgMotionFiltered  = 0;
uint64_t Framegen::s_fgMotionFMemory   = 0;
uint64_t Framegen::s_fgMotionPrevImage = 0;
uint64_t Framegen::s_fgMotionPrevMemory= 0;
bool     Framegen::s_fgMotionPrevValid = false;
uint64_t Framegen::s_fgOutputImage     = 0;
uint64_t Framegen::s_fgOutputMemory    = 0;
uint32_t Framegen::s_fgMotionW         = 0;
uint32_t Framegen::s_fgMotionH         = 0;

Framegen::FgVulkanFuncs Framegen::s_fgVk;

// ===========================================================================
// FG constants (extracted from dxvk_vegas.cpp)
// ===========================================================================

namespace {
  enum : uint32_t {
    FG_PASS_MOTION  = 0,
    FG_PASS_MEDIAN  = 1,
    FG_PASS_WARP    = 2,

    FG_BIND_CURRENT     = 0,
    FG_BIND_PREVIOUS    = 1,
    FG_BIND_MOTION      = 2,
    FG_BIND_OUTPUT      = 3,
    FG_BIND_MOTION_PREV = 4,

    FG_DESC_POOL_SIZE  = 3,
    FG_TILE_SIZE       = 16,
    FG_MEDIAN_TILE     = 8,
    FG_WARP_TILE       = 8,
  };
} // anonymous namespace

// ===========================================================================
// needsFrameGen (with user toggle)
// ===========================================================================

bool Framegen::needsFrameGen(float frameTimeMs, uint32_t tier) {
  // User toggle: vegas.enableFramegen = True force-enables framegen
  // regardless of tier (e.g. Tier 1 testing).
  if (s_toggle == Tristate::True)
    return true;
  if (tier == 1) return false;
  if (tier == 2) return frameTimeMs <= 29.0f;  // >=34 FPS headroom
  return frameTimeMs <= 33.0f;                   // >=30 FPS headroom
}

  static bool initFgPipeline(VkDevice device) {
    if (s_fgInitialized)
      return reinterpret_cast<VkPipeline>(s_fgPipeline[0]) != VK_NULL_HANDLE;

    VkResult vr;

    // ---- Shader modules ----
    const struct { const uint32_t* code; size_t size; } shaders[3] = {
      { dxvk_fg_motion_code,  sizeof(dxvk_fg_motion_code)  },
      { dxvk_fg_median_code,  sizeof(dxvk_fg_median_code)  },
      { dxvk_fg_warp_code,    sizeof(dxvk_fg_warp_code)    },
    };

    VkShaderModule modules[3] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
    for (uint32_t i = 0; i < 3; i++) {
      VkShaderModuleCreateInfo smCI = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
      smCI.codeSize = shaders[i].size;
      smCI.pCode    = shaders[i].code;
      vr = s_fgVk.vkCreateShaderModule(device, &smCI, nullptr, &modules[i]);
      if (vr != VK_SUCCESS) {
        Logger::warn(str::format("Vegas FG: vkCreateShaderModule(pass ", i, ") failed (", vr, ")"));
        for (uint32_t j = 0; j < i; j++)
          s_fgVk.vkDestroyShaderModule(device, modules[j], nullptr);
        s_fgInitialized = true;
        return false;
      }
    }

    // ---- Descriptor set layout (5 bindings, shared) ----
    VkDescriptorSetLayoutBinding bindings[5] = {};
    // Binding 0: uCurrent (sampled)
    bindings[0].binding            = FG_BIND_CURRENT;
    bindings[0].descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[0].descriptorCount    = 1;
    bindings[0].stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT;
    // Binding 1: uPrevious (sampled)
    bindings[1].binding            = FG_BIND_PREVIOUS;
    bindings[1].descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[1].descriptorCount    = 1;
    bindings[1].stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT;
    // Binding 2: motion/median (storage)
    bindings[2].binding            = FG_BIND_MOTION;
    bindings[2].descriptorType     = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorCount    = 1;
    bindings[2].stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT;
    // Binding 3: median output / warp output (storage)
    bindings[3].binding            = FG_BIND_OUTPUT;
    bindings[3].descriptorType     = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[3].descriptorCount    = 1;
    bindings[3].stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT;
    // Binding 4: previous frame's filtered motion (sampled, search center)
    bindings[4].binding            = FG_BIND_MOTION_PREV;
    bindings[4].descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[4].descriptorCount    = 1;
    bindings[4].stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo dslCI = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dslCI.bindingCount = 5;
    dslCI.pBindings    = bindings;

    VkDescriptorSetLayout dsLayout = VK_NULL_HANDLE;
    vr = s_fgVk.vkCreateDescriptorSetLayout(device, &dslCI, nullptr, &dsLayout);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkCreateDescriptorSetLayout failed (", vr, ")"));
      for (uint32_t i = 0; i < 3; i++)
        s_fgVk.vkDestroyShaderModule(device, modules[i], nullptr);
      s_fgInitialized = true;
      return false;
    }

    // ---- Set 1: bimodal-gate diagnostic stats buffer ----
    // One storage-buffer binding read by the motion pass only (thread-0
    // per block, atomicAdd).  Own layout + own pool so the shared
    // 5-binding image layout and pool stay exactly as OOM-tuned.
    VkDescriptorSetLayoutBinding statsBindings[1] = {};
    statsBindings[0].binding            = 0;
    statsBindings[0].descriptorType     = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    statsBindings[0].descriptorCount    = 1;
    statsBindings[0].stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo statsDSLCI = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    statsDSLCI.bindingCount = 1;
    statsDSLCI.pBindings    = statsBindings;

    VkDescriptorSetLayout statsLayout = VK_NULL_HANDLE;
    vr = s_fgVk.vkCreateDescriptorSetLayout(device, &statsDSLCI, nullptr, &statsLayout);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkCreateDescriptorSetLayout(stats) failed (", vr, ")"));
      s_fgVk.vkDestroyDescriptorSetLayout(device, dsLayout, nullptr);
      for (uint32_t i = 0; i < 3; i++)
        s_fgVk.vkDestroyShaderModule(device, modules[i], nullptr);
      s_fgInitialized = true;
      return false;
    }

    // ---- Pipeline layout (push constants + image set 0 + stats set 1) ----
    VkPushConstantRange pcRange = {};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(float) * 8;  // 2x vec4: info + adaptive blend

    VkDescriptorSetLayout plLayouts[2] = { dsLayout, statsLayout };
    VkPipelineLayoutCreateInfo plCI = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plCI.setLayoutCount         = 2;
    plCI.pSetLayouts            = plLayouts;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges    = &pcRange;

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    vr = s_fgVk.vkCreatePipelineLayout(device, &plCI, nullptr, &pipelineLayout);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkCreatePipelineLayout failed (", vr, ")"));
      s_fgVk.vkDestroyDescriptorSetLayout(device, dsLayout, nullptr);
      s_fgVk.vkDestroyDescriptorSetLayout(device, statsLayout, nullptr);
      for (uint32_t i = 0; i < 3; i++)
        s_fgVk.vkDestroyShaderModule(device, modules[i], nullptr);
      s_fgInitialized = true;
      return false;
    }

    // ---- Compute pipelines ----
    VkComputePipelineCreateInfo cpCI[3] = {};
    for (uint32_t i = 0; i < 3; i++) {
      cpCI[i].sType              = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
      cpCI[i].stage.sType        = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      cpCI[i].stage.stage        = VK_SHADER_STAGE_COMPUTE_BIT;
      cpCI[i].stage.module       = modules[i];
      cpCI[i].stage.pName        = "main";
      cpCI[i].layout             = pipelineLayout;
    }

    VkPipeline pipelines[3] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
    vr = s_fgVk.vkCreateComputePipelines(device, VK_NULL_HANDLE, 3, cpCI, nullptr, pipelines);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkCreateComputePipelines failed (", vr, ")"));
      s_fgVk.vkDestroyDescriptorSetLayout(device, statsLayout, nullptr);
      s_fgVk.vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
      s_fgVk.vkDestroyDescriptorSetLayout(device, dsLayout, nullptr);
      for (uint32_t i = 0; i < 3; i++)
        s_fgVk.vkDestroyShaderModule(device, modules[i], nullptr);
      s_fgInitialized = true;
      return false;
    }

    // ---- Destroy shader modules (no longer needed) ----
    for (uint32_t i = 0; i < 3; i++)
      s_fgVk.vkDestroyShaderModule(device, modules[i], nullptr);

    // ---- Descriptor pool ----
    // NOTE: all 3 sets share ONE layout (3 sampled + 2 storage bindings),
    // so pool accounting is per-set full-layout: 3*3 = 9 sampled, 3*2 = 6 storage.
    // Allocate with headroom for driver exact-fit quirks.
    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    poolSizes[0].descriptorCount = FG_DESC_POOL_SIZE * 4;  // 9 needed (shared layout), 12 with headroom
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = FG_DESC_POOL_SIZE * 3;  // 6 needed, 9 with headroom

    VkDescriptorPoolCreateInfo dpCI = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpCI.maxSets       = FG_DESC_POOL_SIZE;
    dpCI.poolSizeCount = 2;
    dpCI.pPoolSizes    = poolSizes;

    VkDescriptorPool descPool = VK_NULL_HANDLE;
    vr = s_fgVk.vkCreateDescriptorPool(device, &dpCI, nullptr, &descPool);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkCreateDescriptorPool failed (", vr, ")"));
      s_fgVk.vkDestroyPipeline(device, pipelines[0], nullptr);
      s_fgVk.vkDestroyPipeline(device, pipelines[1], nullptr);
      s_fgVk.vkDestroyPipeline(device, pipelines[2], nullptr);
      s_fgVk.vkDestroyDescriptorSetLayout(device, statsLayout, nullptr);
      s_fgVk.vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
      s_fgVk.vkDestroyDescriptorSetLayout(device, dsLayout, nullptr);
      s_fgInitialized = true;
      return false;
    }

    // ---- Stats buffer (set 1 / binding 0) ----
    // Single 16-byte host-visible buffer: { u32 count, u32 sumQ16, u32 zero, u32 full }.
    // Written only by motion block-thread-0 atomicAdd; host-visible+coherent so
    // the readback after vkWaitForFences needs no invalidate call.
    // TELEMETRY GATE: the whole diagnostic path (fill, barrier, readback, log)
    // runs only when env vegas_telemetry=1 (default OFF). The buffer + descriptor
    // set are still created so the motion shader's atomicAdds always have a valid
    // target — the gate skips the per-dispatch fill/bind cost and the readback log.
    {
      constexpr uint32_t FG_STATS_WORDS = 4;      // count, sumQ16, zero, full

      VkBufferCreateInfo bufCI = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
      bufCI.size               = FG_STATS_WORDS * sizeof(uint32_t);
      bufCI.usage              = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      bufCI.sharingMode        = VK_SHARING_MODE_EXCLUSIVE;

      VkBuffer statsBuf = VK_NULL_HANDLE;
      vr = s_fgVk.vkCreateBuffer(device, &bufCI, nullptr, &statsBuf);
      if (vr != VK_SUCCESS) {
        Logger::warn(str::format("Vegas FG: vkCreateBuffer(stats) failed (", vr, ")"));
        s_fgVk.vkDestroyDescriptorPool(device, descPool, nullptr);
        s_fgVk.vkDestroyPipeline(device, pipelines[0], nullptr);
        s_fgVk.vkDestroyPipeline(device, pipelines[1], nullptr);
        s_fgVk.vkDestroyPipeline(device, pipelines[2], nullptr);
        s_fgVk.vkDestroyDescriptorSetLayout(device, statsLayout, nullptr);
        s_fgVk.vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        s_fgVk.vkDestroyDescriptorSetLayout(device, dsLayout, nullptr);
        s_fgInitialized = true;
        return false;
      }

      VkMemoryRequirements memReq;
      s_fgVk.vkGetBufferMemoryRequirements(device, statsBuf, &memReq);

      VkPhysicalDeviceMemoryProperties memProps;
      s_fgVk.vkGetPhysicalDeviceMemoryProperties(
          reinterpret_cast<VkPhysicalDevice>(s_physicalDevice), &memProps);

      uint32_t memType = VK_MAX_MEMORY_TYPES;
      for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((memReq.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) {
          memType = i;
          break;
        }
      }
      if (memType == VK_MAX_MEMORY_TYPES) {
        Logger::warn("Vegas FG: no host-visible memory type for stats buffer");
        s_fgVk.vkDestroyBuffer(device, statsBuf, nullptr);
        s_fgVk.vkDestroyDescriptorPool(device, descPool, nullptr);
        s_fgVk.vkDestroyPipeline(device, pipelines[0], nullptr);
        s_fgVk.vkDestroyPipeline(device, pipelines[1], nullptr);
        s_fgVk.vkDestroyPipeline(device, pipelines[2], nullptr);
        s_fgVk.vkDestroyDescriptorSetLayout(device, statsLayout, nullptr);
        s_fgVk.vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        s_fgVk.vkDestroyDescriptorSetLayout(device, dsLayout, nullptr);
        s_fgInitialized = true;
        return false;
      }

      VkMemoryAllocateInfo allocAI = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
      allocAI.allocationSize  = memReq.size;
      allocAI.memoryTypeIndex = memType;

      VkDeviceMemory mem = VK_NULL_HANDLE;
      vr = s_fgVk.vkAllocateMemory(device, &allocAI, nullptr, &mem);
      if (vr != VK_SUCCESS) {
        Logger::warn(str::format("Vegas FG: vkAllocateMemory(stats) failed (", vr, ")"));
        s_fgVk.vkDestroyBuffer(device, statsBuf, nullptr);
        s_fgVk.vkDestroyDescriptorPool(device, descPool, nullptr);
        s_fgVk.vkDestroyPipeline(device, pipelines[0], nullptr);
        s_fgVk.vkDestroyPipeline(device, pipelines[1], nullptr);
        s_fgVk.vkDestroyPipeline(device, pipelines[2], nullptr);
        s_fgVk.vkDestroyDescriptorSetLayout(device, statsLayout, nullptr);
        s_fgVk.vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        s_fgVk.vkDestroyDescriptorSetLayout(device, dsLayout, nullptr);
        s_fgInitialized = true;
        return false;
      }

      vr = s_fgVk.vkBindBufferMemory(device, statsBuf, mem, 0);
      if (vr != VK_SUCCESS) {
        Logger::warn(str::format("Vegas FG: vkBindBufferMemory(stats) failed (", vr, ")"));
        s_fgVk.vkFreeMemory(device, mem, nullptr);
        s_fgVk.vkDestroyBuffer(device, statsBuf, nullptr);
        s_fgVk.vkDestroyDescriptorPool(device, descPool, nullptr);
        s_fgVk.vkDestroyPipeline(device, pipelines[0], nullptr);
        s_fgVk.vkDestroyPipeline(device, pipelines[1], nullptr);
        s_fgVk.vkDestroyPipeline(device, pipelines[2], nullptr);
        s_fgVk.vkDestroyDescriptorSetLayout(device, statsLayout, nullptr);
        s_fgVk.vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        s_fgVk.vkDestroyDescriptorSetLayout(device, dsLayout, nullptr);
        s_fgInitialized = true;
        return false;
      }

      void* mapPtr = nullptr;
      vr = s_fgVk.vkMapMemory(device, mem, 0, VK_WHOLE_SIZE, 0, &mapPtr);
      if (vr != VK_SUCCESS) {
        Logger::warn(str::format("Vegas FG: vkMapMemory(stats) failed (", vr, ")"));
        s_fgVk.vkFreeMemory(device, mem, nullptr);
        s_fgVk.vkDestroyBuffer(device, statsBuf, nullptr);
        s_fgVk.vkDestroyDescriptorPool(device, descPool, nullptr);
        s_fgVk.vkDestroyPipeline(device, pipelines[0], nullptr);
        s_fgVk.vkDestroyPipeline(device, pipelines[1], nullptr);
        s_fgVk.vkDestroyPipeline(device, pipelines[2], nullptr);
        s_fgVk.vkDestroyDescriptorSetLayout(device, statsLayout, nullptr);
        s_fgVk.vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        s_fgVk.vkDestroyDescriptorSetLayout(device, dsLayout, nullptr);
        s_fgInitialized = true;
        return false;
      }
      memset(mapPtr, 0, FG_STATS_WORDS * sizeof(uint32_t));
      s_fgStatsEnabled = (env::getEnvVar("vegas_telemetry") == "1");

      // Own pool for the single stats set (no touch to the OOM-tuned pool)
      VkDescriptorPoolSize statsPoolSizes[1] = {};
      statsPoolSizes[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      statsPoolSizes[0].descriptorCount = 1;

      VkDescriptorPoolCreateInfo statsPoolCI = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
      statsPoolCI.maxSets       = 1;
      statsPoolCI.poolSizeCount = 1;
      statsPoolCI.pPoolSizes    = statsPoolSizes;

      VkDescriptorPool statsPool = VK_NULL_HANDLE;
      vr = s_fgVk.vkCreateDescriptorPool(device, &statsPoolCI, nullptr, &statsPool);
      if (vr != VK_SUCCESS) {
        Logger::warn(str::format("Vegas FG: vkCreateDescriptorPool(stats) failed (", vr, ")"));
        s_fgVk.vkUnmapMemory(device, mem);
        s_fgVk.vkFreeMemory(device, mem, nullptr);
        s_fgVk.vkDestroyBuffer(device, statsBuf, nullptr);
        s_fgVk.vkDestroyDescriptorPool(device, descPool, nullptr);
        s_fgVk.vkDestroyPipeline(device, pipelines[0], nullptr);
        s_fgVk.vkDestroyPipeline(device, pipelines[1], nullptr);
        s_fgVk.vkDestroyPipeline(device, pipelines[2], nullptr);
        s_fgVk.vkDestroyDescriptorSetLayout(device, statsLayout, nullptr);
        s_fgVk.vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        s_fgVk.vkDestroyDescriptorSetLayout(device, dsLayout, nullptr);
        s_fgInitialized = true;
        return false;
      }

      VkDescriptorSetAllocateInfo statsAlloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
      statsAlloc.descriptorPool     = statsPool;
      statsAlloc.descriptorSetCount = 1;
      statsAlloc.pSetLayouts        = &statsLayout;

      VkDescriptorSet statsSet = VK_NULL_HANDLE;
      vr = s_fgVk.vkAllocateDescriptorSets(device, &statsAlloc, &statsSet);
      if (vr != VK_SUCCESS) {
        Logger::warn(str::format("Vegas FG: vkAllocateDescriptorSets(stats) failed (", vr, ")"));
        s_fgVk.vkDestroyDescriptorPool(device, statsPool, nullptr);
        s_fgVk.vkUnmapMemory(device, mem);
        s_fgVk.vkFreeMemory(device, mem, nullptr);
        s_fgVk.vkDestroyBuffer(device, statsBuf, nullptr);
        s_fgVk.vkDestroyDescriptorPool(device, descPool, nullptr);
        s_fgVk.vkDestroyPipeline(device, pipelines[0], nullptr);
        s_fgVk.vkDestroyPipeline(device, pipelines[1], nullptr);
        s_fgVk.vkDestroyPipeline(device, pipelines[2], nullptr);
        s_fgVk.vkDestroyDescriptorSetLayout(device, statsLayout, nullptr);
        s_fgVk.vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        s_fgVk.vkDestroyDescriptorSetLayout(device, dsLayout, nullptr);
        s_fgInitialized = true;
        return false;
      }

      VkDescriptorBufferInfo statsBufInfo = {};
      statsBufInfo.buffer = statsBuf;
      statsBufInfo.offset = 0;
      statsBufInfo.range  = FG_STATS_WORDS * sizeof(uint32_t);

      VkWriteDescriptorSet statsWrite = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
      statsWrite.dstSet          = statsSet;
      statsWrite.dstBinding      = 0;
      statsWrite.dstArrayElement = 0;
      statsWrite.descriptorCount = 1;
      statsWrite.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      statsWrite.pBufferInfo     = &statsBufInfo;

      s_fgVk.vkUpdateDescriptorSets(device, 1, &statsWrite, 0, nullptr);

      s_fgStatsBuffer  = reinterpret_cast<uint64_t>(statsBuf);
      s_fgStatsMemory  = reinterpret_cast<uint64_t>(mem);
      s_fgStatsMapping = reinterpret_cast<uint64_t>(mapPtr);
      s_fgStatsLayout  = reinterpret_cast<uint64_t>(statsLayout);
      s_fgStatsPool    = reinterpret_cast<uint64_t>(statsPool);
      s_fgStatsSet     = reinterpret_cast<uint64_t>(statsSet);
      s_fgStatsFrames  = 0;
      s_fgStatsCorrupt = 0;
    }

    // ---- Store as boxed uint64_t ----
    s_fgPipeline[0]     = reinterpret_cast<uint64_t>(pipelines[0]);
    s_fgPipeline[1]     = reinterpret_cast<uint64_t>(pipelines[1]);
    s_fgPipeline[2]     = reinterpret_cast<uint64_t>(pipelines[2]);
    s_fgPipelineLayout  = reinterpret_cast<uint64_t>(pipelineLayout);
    s_fgDescSetLayout   = reinterpret_cast<uint64_t>(dsLayout);
    s_fgDescPool        = reinterpret_cast<uint64_t>(descPool);

    s_fgInitialized = true;

    Logger::debug("Vegas FG: 3-pass pipeline initialized");
    return true;
  }

  /** Helper: ensure framegen intermediate images exist at given resolution.
   *  Creates s_fgPrevImage, s_fgMotionImage, s_fgMotionFiltered, s_fgOutputImage
   *  if dimensions changed or images do not exist.
   */
  static bool ensureFgIntermediateImages(VkDevice device, uint32_t w, uint32_t h, VkFormat format) {
    VkResult vr;

    // Motion buffer dimensions: one vector per 16×16 tile
    uint32_t mw = (w + FG_TILE_SIZE - 1) / FG_TILE_SIZE;
    uint32_t mh = (h + FG_TILE_SIZE - 1) / FG_TILE_SIZE;

    // If dimensions match and images exist, nothing to do
    if (s_fgPrevImage && s_fgMotionImage && s_fgMotionFiltered && s_fgOutputImage
        && s_fgMotionPrevImage
        && s_fgPrevW == w && s_fgPrevH == h && s_fgMotionW == mw && s_fgMotionH == mh
        && s_fgFormat == format)
      return true;

    // ---- Destroy old images if any ----
    auto destroyImage = [&](uint64_t& img, uint64_t& mem) {
      if (img) {
        s_fgVk.vkDestroyImage(device, reinterpret_cast<VkImage>(img), nullptr);
        img = 0;
      }
      if (mem) {
        s_fgVk.vkFreeMemory(device, reinterpret_cast<VkDeviceMemory>(mem), nullptr);
        mem = 0;
      }
    };

    destroyImage(s_fgPrevImage,       s_fgPrevMemory);
    destroyImage(s_fgMotionImage,     s_fgMotionMemory);
    destroyImage(s_fgMotionFiltered,  s_fgMotionFMemory);
    destroyImage(s_fgMotionPrevImage, s_fgMotionPrevMemory);
    destroyImage(s_fgOutputImage,     s_fgOutputMemory);

    s_fgPrevValid = false;
    s_fgMotionPrevValid = false;
    s_fgPrevW = 0;
    s_fgPrevH = 0;
    s_fgMotionW = 0;
    s_fgMotionH = 0;
    s_fgFormat = VK_FORMAT_UNDEFINED;

    // Helper to create a storage/transfer image
    auto createImage = [&](uint32_t imgW, uint32_t imgH,
                           VkFormat fmt, VkImageUsageFlags usage,
                           uint64_t& outImg, uint64_t& outMem) -> bool {
      VkImageCreateInfo imgCI = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
      imgCI.imageType     = VK_IMAGE_TYPE_2D;
      imgCI.extent.width  = imgW;
      imgCI.extent.height = imgH;
      imgCI.extent.depth  = 1;
      imgCI.mipLevels     = 1;
      imgCI.arrayLayers   = 1;
      imgCI.format        = fmt;
      imgCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
      imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      imgCI.usage         = usage;
      imgCI.samples       = VK_SAMPLE_COUNT_1_BIT;

      VkImage img = VK_NULL_HANDLE;
      vr = s_fgVk.vkCreateImage(device, &imgCI, nullptr, &img);
      if (vr != VK_SUCCESS) {
        Logger::warn(str::format("Vegas FG: vkCreateImage (", imgW, "x", imgH, ") failed (", vr, ")"));
        return false;
      }

      VkMemoryRequirements memReq;
      s_fgVk.vkGetImageMemoryRequirements(device, img, &memReq);

      VkPhysicalDeviceMemoryProperties memProps;
      s_fgVk.vkGetPhysicalDeviceMemoryProperties(
          reinterpret_cast<VkPhysicalDevice>(s_physicalDevice), &memProps);

      uint32_t memType = VK_MAX_MEMORY_TYPES;
      for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((memReq.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
          memType = i;
          break;
        }
      }
      if (memType == VK_MAX_MEMORY_TYPES) {
        Logger::warn("Vegas FG: no suitable memory type for intermediate image");
        s_fgVk.vkDestroyImage(device, img, nullptr);
        return false;
      }

      VkMemoryAllocateInfo allocAI = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
      allocAI.allocationSize  = memReq.size;
      allocAI.memoryTypeIndex = memType;

      VkDeviceMemory mem = VK_NULL_HANDLE;
      vr = s_fgVk.vkAllocateMemory(device, &allocAI, nullptr, &mem);
      if (vr != VK_SUCCESS) {
        Logger::warn(str::format("Vegas FG: vkAllocateMemory failed (", vr, ")"));
        s_fgVk.vkDestroyImage(device, img, nullptr);
        return false;
      }

      vr = s_fgVk.vkBindImageMemory(device, img, mem, 0);
      if (vr != VK_SUCCESS) {
        Logger::warn(str::format("Vegas FG: vkBindImageMemory failed (", vr, ")"));
        s_fgVk.vkFreeMemory(device, mem, nullptr);
        s_fgVk.vkDestroyImage(device, img, nullptr);
        return false;
      }

      outImg = reinterpret_cast<uint64_t>(img);
      outMem = reinterpret_cast<uint64_t>(mem);
      return true;
    };

    // ---- Create images ----
    // s_fgPrevImage: previous frame (same format as swapchain so the
    // cur→prev copy is a legal same-format vkCmdCopyImage)
    if (!createImage(w, h, format,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        s_fgPrevImage, s_fgPrevMemory)) {
      return false;
    }

    // s_fgMotionImage: raw motion vectors + block SAD confidence
    // (R16G16B16A16_SFLOAT — matches shaders' Rgba16f storage format;
    // xy = motion vector, z = normalized block SAD ∈ [0,1])
    if (!createImage(mw, mh, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        s_fgMotionImage, s_fgMotionMemory)) {
      return false;
    }

    // s_fgMotionFiltered: median-filtered motion + min-SAD (R16G16B16A16_SFLOAT)
    if (!createImage(mw, mh, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        s_fgMotionFiltered, s_fgMotionFMemory)) {
      return false;
    }

    // s_fgMotionPrevImage: previous frame's filtered motion — temporal
    // search center for the next frame's motion pass (R16G16B16A16_SFLOAT)
    if (!createImage(mw, mh, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        s_fgMotionPrevImage, s_fgMotionPrevMemory)) {
      return false;
    }

    // s_fgOutputImage: interpolated frame output (swapchain format so the
    // final fgOutput → curImage blit is same-format; the warp shader's
    // uOutput declares Unknown format, see star_fg_spv.h)
    if (!createImage(w, h, format,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        s_fgOutputImage, s_fgOutputMemory)) {
      return false;
    }

    s_fgPrevW   = w;
    s_fgPrevH   = h;
    s_fgMotionW = mw;
    s_fgMotionH = mh;
    s_fgFormat  = format;

    Logger::debug(str::format("Vegas FG: intermediate images created (",
                              w, "x", h, ", motion ", mw, "x", mh, ")"));
    return true;
  }

  /** Public getter for framegen output image (uint64_t → VkImage). */
  uint64_t Framegen::framegenOutputImage() {
    return s_fgOutputImage;
  }

  /** Returns true if the Frame Generator has a valid VkDevice/VkQueue. */
  bool Framegen::isFrameGenReady() {
    // User toggle: vegas.enableFramegen = False disables framegen entirely,
    // irrespective of tier. True is handled in needsFrameGen (force-enable).
    auto dev = nullptr; // host-provided; toggle read via s_toggle
    if (dev != nullptr && s_toggle == Tristate::False)
      return false;
    return s_device != nullptr;
  }

  /** Dispatch 3-pass motion-compensated framegen.
   *
   *  \param [in] curImage  Current rendered frame (VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
   *  \param [in] prevImage Previous frame (caller-provided; may be VK_NULL_HANDLE)
   *  \param [in] extent    Image dimensions
   *  \param [in] format    Image format (must be R8G8B8A8_UNORM)
  static void fgDestroyPendingEntry(VkDevice device, uint32_t idx) {
    FgPendingResources& p = s_fgPending[idx];
    for (uint32_t v = 0; v < p.viewCount; v++)
      s_fgVk.vkDestroyImageView(device, p.views[v], nullptr);
    s_fgVk.vkDestroyFence(device, p.fence, nullptr);
    s_fgVk.vkFreeCommandBuffers(device, p.pool, 1, &p.cmdBuf);
    s_fgVk.vkDestroyCommandPool(device, p.pool, nullptr);
    s_fgPending[idx] = s_fgPending[--s_fgPendingCount];
  }

  // Drain entries whose fence has signalled.  Called at the top of
  // framegenDispatch, before any pool reset or allocation, so resources
  // from a previous timeout are freed before new submissions race them.
  static void fgDrainPending(VkDevice device) {
    uint32_t i = 0;
    while (i < s_fgPendingCount) {
      VkResult vr = s_fgVk.vkWaitForFences(device, 1,
          &s_fgPending[i].fence, VK_TRUE, 0);
      if (vr == VK_SUCCESS)
        fgDestroyPendingEntry(device, i);   // re-check slot i
      else
        i++;                                // still in flight, keep parked
    }
  }

  // Park in-flight resources for deferred destruction after a fence
  // timeout.  If the list is full, block on the oldest entry until it
  // signals (bounded only by device health; VK_ERROR_DEVICE_LOST returns
  // instead of hanging).  On persistent device-lost we leak rather than
  // risk destroying still-executing objects.
  static void fgQueuePending(VkDevice device, VkFence fence,
      VkCommandPool pool, VkCommandBuffer cmdBuf,
      const VkImageView* views, uint32_t viewCount) {
    if (s_fgPendingCount >= 4) {
      VkResult vr = s_fgVk.vkWaitForFences(device, 1,
          &s_fgPending[0].fence, VK_TRUE, UINT64_MAX);
      if (vr == VK_SUCCESS) {
        fgDestroyPendingEntry(device, 0);
      } else {
        Logger::warn(str::format(
            "Vegas FG: pending list full, oldest wait failed (", vr,
            ") — deferring cleanup"));
        return;
      }
    }
    if (s_fgPendingCount < 4) {
      FgPendingResources& p = s_fgPending[s_fgPendingCount++];
      p = FgPendingResources();
      p.fence     = fence;
      p.pool      = pool;
      p.cmdBuf    = cmdBuf;
      p.viewCount = viewCount;
      for (uint32_t v = 0; v < viewCount; v++)
        p.views[v] = views[v];
    }
  }

  bool Framegen::framegenDispatch(
          VkImage              curImage,
          VkImage              prevImage,
          VkExtent3D           extent,
          VkFormat             format,
          bool                 hudRectValid,
    const float*               hudRect) {
    // HUD graph rect → motion block coords (one block per 16×16 tile).
    // Sentinel: x0 > x1 disables the mask.
    int hudX0 = 1, hudY0 = 1, hudX1 = 0, hudY1 = 0;
    if (hudRectValid && hudRect != nullptr) {
      const float invTile = 1.0f / float(FG_TILE_SIZE);
      hudX0 = std::max(int(hudRect[0] * invTile) - 1, 0);
      hudY0 = std::max(int(hudRect[1] * invTile) - 1, 0);
      hudX1 = int(hudRect[2] * invTile) + 1;
      hudY1 = int(hudRect[3] * invTile) + 1;
    }
    // ================================================================
    // Guard: only UNORM supported
    // ================================================================
    if (format != VK_FORMAT_R8G8B8A8_UNORM &&
        format != VK_FORMAT_B8G8R8A8_UNORM) {
      Logger::debug("Vegas FG: skipped — unsupported format");
      return false;
    }

    // ================================================================
    // Get device & queue
    // ================================================================
    VkDevice device = reinterpret_cast<VkDevice>(s_device);
    VkQueue  queue  = reinterpret_cast<VkQueue>(s_queue);
    if (device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE) {
      Logger::debug("Vegas FG: skipped — no VkDevice/VkQueue");
      return false;
    }

    if (!fgLoadVulkanFuncs(device)) {
      Logger::debug("Vegas FG: skipped — Vulkan functions not available");
      return false;
    }

    // ================================================================
    // Saturation skip (adaptive dispatch).  The watchdog (s_fgSlowCount,
    // incremented on >25ms fence waits, capped at 5) tripping means the
    // shared graphics queue is saturated: the 50ms dispatch fence wait
    // stalls the present thread and FG-on runs SLOWER than FG-off (TR13
    // test, 2026-08-03).  Present native instead — no motion/warp/blend
    // dispatch, no fence wait, no parked CB.  Re-arm: after
    // FG_SKIP_WINDOW skipped frames run one probe dispatch (slowCount
    // reset to 4 so a single slow wait re-trips immediately; a clean
    // wait decays normally and FG stays on).  s_fgPrevValid is cleared
    // so the probe frame takes the capture path below — fresh prev
    // reference, native present, then normal generation resumes.
    // ================================================================
    if (s_fgSlowCount >= 5) {
      if (s_fgSkipFrames == 0)
        Logger::debug("Vegas FG: saturated — presenting native (skip window)");
      s_fgSkipFrames++;
      if (s_fgSkipFrames < FG_SKIP_WINDOW)
        return false;
      s_fgSkipFrames = 0;
      s_fgSlowCount  = 4;
      s_fgPrevValid  = false;
      Logger::debug("Vegas FG: re-arm probe — resuming framegen");
    }

    // Free resources from a previous fence-timeout before this frame
    // allocates/resets anything (prevents reuse races with in-flight work).
    fgDrainPending(device);

    if (!initFgPipeline(device)) {
      Logger::debug("Vegas FG: skipped — pipeline init failed");
      return false;
    }

    if (!ensureFgIntermediateImages(device, extent.width, extent.height, format)) {
      Logger::debug("Vegas FG: skipped — intermediate image creation failed");
      return false;
    }

    VkPipelineLayout    pipelineLayout  = reinterpret_cast<VkPipelineLayout>(s_fgPipelineLayout);
    VkDescriptorSetLayout descSetLayout = reinterpret_cast<VkDescriptorSetLayout>(s_fgDescSetLayout);
    VkDescriptorPool    descPool        = reinterpret_cast<VkDescriptorPool>(s_fgDescPool);

    // ================================================================
    // First frame? Just save current as previous, return false
    // ================================================================
    if (prevImage == VK_NULL_HANDLE && !s_fgPrevValid) {
      // Copy curImage → s_fgPrevImage for next frame
      // Use a simple command buffer for the copy
      VkCommandPoolCreateInfo poolCI = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
      poolCI.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
      poolCI.queueFamilyIndex = s_queueFamily;
      VkCommandPool cmdPool = VK_NULL_HANDLE;
      VkResult vr = s_fgVk.vkCreateCommandPool(device, &poolCI, nullptr, &cmdPool);
      if (vr != VK_SUCCESS) return false;

      VkCommandBufferAllocateInfo allocCI = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
      allocCI.commandPool        = cmdPool;
      allocCI.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      allocCI.commandBufferCount = 1;
      VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
      vr = s_fgVk.vkAllocateCommandBuffers(device, &allocCI, &cmdBuf);
      if (vr != VK_SUCCESS) {
        s_fgVk.vkDestroyCommandPool(device, cmdPool, nullptr);
        return false;
      }

      VkFenceCreateInfo fenceCI = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
      VkFence fence = VK_NULL_HANDLE;
      vr = s_fgVk.vkCreateFence(device, &fenceCI, nullptr, &fence);
      if (vr != VK_SUCCESS) {
        s_fgVk.vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
        s_fgVk.vkDestroyCommandPool(device, cmdPool, nullptr);
        return false;
      }

      VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
      beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      s_fgVk.vkBeginCommandBuffer(cmdBuf, &beginInfo);

      // Transition curImage PRESENT_SRC_KHR → TRANSFER_SRC_OPTIMAL
      VkImageMemoryBarrier curToSrc = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
      curToSrc.srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      curToSrc.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
      curToSrc.oldLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      curToSrc.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      curToSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      curToSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      curToSrc.image               = curImage;
      curToSrc.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

      s_fgVk.vkCmdPipelineBarrier(cmdBuf,
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT,
          0, 0, nullptr, 0, nullptr, 1, &curToSrc);

      // Transition s_fgPrevImage UNDEFINED → TRANSFER_DST_OPTIMAL
      VkImage prevDst = reinterpret_cast<VkImage>(s_fgPrevImage);
      VkImageMemoryBarrier prevToDst = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
      prevToDst.srcAccessMask        = 0;
      prevToDst.dstAccessMask        = VK_ACCESS_TRANSFER_WRITE_BIT;
      prevToDst.oldLayout            = VK_IMAGE_LAYOUT_UNDEFINED;
      prevToDst.newLayout            = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      prevToDst.srcQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
      prevToDst.dstQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
      prevToDst.image                = prevDst;
      prevToDst.subresourceRange     = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

      s_fgVk.vkCmdPipelineBarrier(cmdBuf,
          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT,
          0, 0, nullptr, 0, nullptr, 1, &prevToDst);

      // Copy curImage → prevImage
      VkImageCopy copyRegion = {};
      copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      copyRegion.srcSubresource.layerCount = 1;
      copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      copyRegion.dstSubresource.layerCount = 1;
      copyRegion.extent = extent;

      s_fgVk.vkCmdCopyImage(cmdBuf,
          curImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          prevDst,  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          1, &copyRegion);

      // Restore curImage TRANSFER_SRC_OPTIMAL → PRESENT_SRC_KHR
      VkImageMemoryBarrier curBack = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
      curBack.srcAccessMask        = VK_ACCESS_TRANSFER_READ_BIT;
      curBack.dstAccessMask        = 0;
      curBack.oldLayout            = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      curBack.newLayout            = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      curBack.srcQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
      curBack.dstQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
      curBack.image                = curImage;
      curBack.subresourceRange     = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

      // Leave prev in GENERAL for shader read next frame
      VkImageMemoryBarrier prevToGen = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
      prevToGen.srcAccessMask        = VK_ACCESS_TRANSFER_WRITE_BIT;
      prevToGen.dstAccessMask        = VK_ACCESS_SHADER_READ_BIT;
      prevToGen.oldLayout            = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      prevToGen.newLayout            = VK_IMAGE_LAYOUT_GENERAL;
      prevToGen.srcQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
      prevToGen.dstQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
      prevToGen.image                = prevDst;
      prevToGen.subresourceRange     = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

      VkImageMemoryBarrier postBarriers[2] = { curBack, prevToGen };
      s_fgVk.vkCmdPipelineBarrier(cmdBuf,
          VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          0, 0, nullptr, 0, nullptr, 2, postBarriers);

      s_fgVk.vkEndCommandBuffer(cmdBuf);

      VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
      submitInfo.commandBufferCount = 1;
      submitInfo.pCommandBuffers    = &cmdBuf;
      s_fgVk.vkQueueSubmit(queue, 1, &submitInfo, fence);
      vr = s_fgVk.vkWaitForFences(device, 1, &fence, VK_TRUE, 50'000'000);
      if (vr != VK_SUCCESS) {
        // Bounded wait: fail-closed on a dead/hung queue instead of
        // stalling the present thread forever on the capture path.  The
        // CB may still be executing — park the resources and let a later
        // dispatch destroy them once the fence signals.
        Logger::warn(str::format("Vegas FG: first-frame capture wait failed (", vr, ")"));
        fgQueuePending(device, fence, cmdPool, cmdBuf, nullptr, 0);
        return false;
      }

      s_fgVk.vkDestroyFence(device, fence, nullptr);
      s_fgVk.vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
      s_fgVk.vkDestroyCommandPool(device, cmdPool, nullptr);

      s_fgPrevValid = true;
      Logger::debug("Vegas FG: first frame captured as previous");
      return false;
    }

    // ================================================================
    // Use s_fgPrevImage as the actual previous frame if prevImage is null
    // ================================================================
    VkImage actualPrev = (prevImage != VK_NULL_HANDLE)
                         ? prevImage
                         : reinterpret_cast<VkImage>(s_fgPrevImage);
    VkImage actualCur  = curImage;

    // Unwrap intermediate images
    VkImage motionRaw      = reinterpret_cast<VkImage>(s_fgMotionImage);
    VkImage motionFiltered = reinterpret_cast<VkImage>(s_fgMotionFiltered);
    VkImage fgOutput       = reinterpret_cast<VkImage>(s_fgOutputImage);
    VkImage prevDst        = reinterpret_cast<VkImage>(s_fgPrevImage);

    // All 3 pipelines
    VkPipeline pipelineMotion  = reinterpret_cast<VkPipeline>(s_fgPipeline[FG_PASS_MOTION]);
    VkPipeline pipelineMedian  = reinterpret_cast<VkPipeline>(s_fgPipeline[FG_PASS_MEDIAN]);
    VkPipeline pipelineWarp    = reinterpret_cast<VkPipeline>(s_fgPipeline[FG_PASS_WARP]);

    VkImageView srcViewCur   = VK_NULL_HANDLE;
    VkImageView srcViewPrev  = VK_NULL_HANDLE;
    VkImageView motionView   = VK_NULL_HANDLE;
    VkImageView motionFilteredView = VK_NULL_HANDLE;
    VkImageView motionPrevView     = VK_NULL_HANDLE;
    VkImageView outputView   = VK_NULL_HANDLE;

    // ================================================================
    // Create temporary command pool + buffer + fence
    // ================================================================
    VkCommandPoolCreateInfo poolCI = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolCI.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolCI.queueFamilyIndex = s_queueFamily;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkResult vr = s_fgVk.vkCreateCommandPool(device, &poolCI, nullptr, &cmdPool);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkCreateCommandPool failed (", vr, ")"));
      return false;
    }

    VkCommandBufferAllocateInfo allocCI = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocCI.commandPool        = cmdPool;
    allocCI.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocCI.commandBufferCount = 1;
    VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
    vr = s_fgVk.vkAllocateCommandBuffers(device, &allocCI, &cmdBuf);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkAllocateCommandBuffers failed (", vr, ")"));
      s_fgVk.vkDestroyCommandPool(device, cmdPool, nullptr);
      return false;
    }

    VkFenceCreateInfo fenceCI = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    vr = s_fgVk.vkCreateFence(device, &fenceCI, nullptr, &fence);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkCreateFence failed (", vr, ")"));
      s_fgVk.vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
      s_fgVk.vkDestroyCommandPool(device, cmdPool, nullptr);
      return false;
    }

    // Cleanup helper — call with stage number indicating what was allocated.
    // Stage: 0=none, 1=cmdPool, 2=cmdBuf, 3=fence,
    //        4=srcViewCur, 5=srcViewPrev, 6=motionView,
    //        7=motionFilteredView, 8=outputView, 9=motionPrevView
    auto fgCleanup = [&](int stage) {
      if (stage >= 9)
        s_fgVk.vkDestroyImageView(device, motionPrevView, nullptr);
      if (stage >= 8)
        s_fgVk.vkDestroyImageView(device, outputView, nullptr);
      if (stage >= 7)
        s_fgVk.vkDestroyImageView(device, motionFilteredView, nullptr);
      if (stage >= 6)
        s_fgVk.vkDestroyImageView(device, motionView, nullptr);
      if (stage >= 5)
        s_fgVk.vkDestroyImageView(device, srcViewPrev, nullptr);
      if (stage >= 4)
        s_fgVk.vkDestroyImageView(device, srcViewCur, nullptr);
      if (stage >= 3)
        s_fgVk.vkDestroyFence(device, fence, nullptr);
      if (stage >= 2)
        s_fgVk.vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
      if (stage >= 1)
        s_fgVk.vkDestroyCommandPool(device, cmdPool, nullptr);
    };

    // ================================================================
    // Create image views
    // ================================================================
    VkImageViewCreateInfo viewCI = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    viewCI.viewType     = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format       = format;
    viewCI.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewCI.subresourceRange.baseMipLevel   = 0;
    viewCI.subresourceRange.levelCount     = 1;
    viewCI.subresourceRange.baseArrayLayer = 0;
    viewCI.subresourceRange.layerCount     = 1;

    // curImage view
    viewCI.image = actualCur;
    vr = s_fgVk.vkCreateImageView(device, &viewCI, nullptr, &srcViewCur);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkCreateImageView(cur) failed (", vr, ")"));
      fgCleanup(3); return false;
    }

    // prevImage view (same format as image)
    viewCI.image = actualPrev;
    viewCI.format = format;  // prev is created in the swapchain format
    vr = s_fgVk.vkCreateImageView(device, &viewCI, nullptr, &srcViewPrev);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkCreateImageView(prev) failed (", vr, ")"));
      fgCleanup(4); return false;
    }

    // Motion raw view (R16G16B16A16_SFLOAT — matches shader Rgba16f)
    viewCI.image  = motionRaw;
    viewCI.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    vr = s_fgVk.vkCreateImageView(device, &viewCI, nullptr, &motionView);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkCreateImageView(motion) failed (", vr, ")"));
      fgCleanup(5); return false;
    }

    // Motion filtered view (R16G16B16A16_SFLOAT)
    viewCI.image  = motionFiltered;
    viewCI.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    vr = s_fgVk.vkCreateImageView(device, &viewCI, nullptr, &motionFilteredView);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkCreateImageView(motionFiltered) failed (", vr, ")"));
      fgCleanup(6); return false;
    }

    // Output view (swapchain format — warp uOutput is Unknown in SPIR-V)
    viewCI.image  = fgOutput;
    viewCI.format = format;
    vr = s_fgVk.vkCreateImageView(device, &viewCI, nullptr, &outputView);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkCreateImageView(output) failed (", vr, ")"));
      fgCleanup(7); return false;
    }

    // Motion-prev view (R16G16B16A16_SFLOAT — temporal search center)
    viewCI.image  = reinterpret_cast<VkImage>(s_fgMotionPrevImage);
    viewCI.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    vr = s_fgVk.vkCreateImageView(device, &viewCI, nullptr, &motionPrevView);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkCreateImageView(motionPrev) failed (", vr, ")"));
      fgCleanup(9); return false;
    }

    // ================================================================
    // Record command buffer — 3-pass dispatch
    // ================================================================
    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vr = s_fgVk.vkBeginCommandBuffer(cmdBuf, &beginInfo);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkBeginCommandBuffer failed (", vr, ")"));
      fgCleanup(8); return false;
    }

    // ----------------------------------------------------------------
    // Pre-dispatch barriers: bring all images to GENERAL layout
    // ----------------------------------------------------------------
    VkImageMemoryBarrier preBarriers[5] = {};

    // curImage: PRESENT_SRC_KHR → GENERAL (shader read)
    preBarriers[0].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    preBarriers[0].srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    preBarriers[0].dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    preBarriers[0].oldLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    preBarriers[0].newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    preBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preBarriers[0].image               = actualCur;
    preBarriers[0].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // actualPrev: GENERAL remains GENERAL (assumed already in GENERAL from previous frame)
    // Just ensure shader-read access is visible
    preBarriers[1].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    preBarriers[1].srcAccessMask       = 0;
    preBarriers[1].dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    preBarriers[1].oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    preBarriers[1].newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    preBarriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preBarriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preBarriers[1].image               = actualPrev;
    preBarriers[1].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // motionRaw: UNDEFINED → GENERAL (storage write)
    preBarriers[2].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    preBarriers[2].srcAccessMask       = 0;
    preBarriers[2].dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    preBarriers[2].oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    preBarriers[2].newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    preBarriers[2].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preBarriers[2].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preBarriers[2].image               = motionRaw;
    preBarriers[2].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // motionFiltered: UNDEFINED → GENERAL (storage write)
    preBarriers[3].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    preBarriers[3].srcAccessMask       = 0;
    preBarriers[3].dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    preBarriers[3].oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    preBarriers[3].newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    preBarriers[3].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preBarriers[3].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preBarriers[3].image               = motionFiltered;
    preBarriers[3].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // fgOutput: UNDEFINED → GENERAL (storage write)
    preBarriers[4].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    preBarriers[4].srcAccessMask       = 0;
    preBarriers[4].dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    preBarriers[4].oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    preBarriers[4].newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    preBarriers[4].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preBarriers[4].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preBarriers[4].image               = fgOutput;
    preBarriers[4].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // Split into two calls: barrier 0 (cur) needs COLOR_ATTACHMENT_OUTPUT srcStage,
    // barriers 1-4 need TOP_OF_PIPE srcStage (no prior producer).
    // Barrier for curImage
    VkImageMemoryBarrier curBarrier = preBarriers[0];
    s_fgVk.vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &curBarrier);

    // Barriers for prev + intermediates
    s_fgVk.vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 4, &preBarriers[1]);

    // ----------------------------------------------------------------
    // Allocate 3 descriptor sets (one per pass)
    // ----------------------------------------------------------------
    s_fgVk.vkResetDescriptorPool(device, descPool, 0);

    VkDescriptorSetAllocateInfo descAlloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    descAlloc.descriptorPool     = descPool;
    descAlloc.descriptorSetCount = 3;

    VkDescriptorSetLayout layouts[3] = { descSetLayout, descSetLayout, descSetLayout };
    descAlloc.pSetLayouts = layouts;

    VkDescriptorSet descSets[3] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
    vr = s_fgVk.vkAllocateDescriptorSets(device, &descAlloc, descSets);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkAllocateDescriptorSets failed (", vr, ")"));
      fgCleanup(8); return false;
    }

    // ---- Write descriptors for Pass 1 (MOTION) ----
    // cur + prev (sampled) → motionRaw (storage)
    VkDescriptorImageInfo curImgInfo  = {};
    curImgInfo.sampler     = VK_NULL_HANDLE;
    curImgInfo.imageView   = srcViewCur;
    curImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo prevImgInfo = {};
    prevImgInfo.sampler     = VK_NULL_HANDLE;
    prevImgInfo.imageView   = srcViewPrev;
    prevImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo motionImgInfo = {};
    motionImgInfo.sampler     = VK_NULL_HANDLE;
    motionImgInfo.imageView   = motionView;
    motionImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo mfiltImgInfo = {};
    mfiltImgInfo.sampler     = VK_NULL_HANDLE;
    mfiltImgInfo.imageView   = motionFilteredView;
    mfiltImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo motionPrevImgInfo = {};
    motionPrevImgInfo.sampler     = VK_NULL_HANDLE;
    motionPrevImgInfo.imageView   = motionPrevView;
    motionPrevImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo outputImgInfo = {};
    outputImgInfo.sampler     = VK_NULL_HANDLE;
    outputImgInfo.imageView   = outputView;
    outputImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[4] = {};

    // Pass 1: bind 0=cur, 1=prev, 2=motion, 3=unused
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = descSets[0];
    writes[0].dstBinding      = FG_BIND_CURRENT;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[0].pImageInfo      = &curImgInfo;

    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = descSets[0];
    writes[1].dstBinding      = FG_BIND_PREVIOUS;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[1].pImageInfo      = &prevImgInfo;

    writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet          = descSets[0];
    writes[2].dstBinding      = FG_BIND_MOTION;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].pImageInfo      = &motionImgInfo;

    writes[3].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet          = descSets[0];
    writes[3].dstBinding      = FG_BIND_MOTION_PREV;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[3].pImageInfo      = &motionPrevImgInfo;

    s_fgVk.vkUpdateDescriptorSets(device, 4, writes, 0, nullptr);

    // Pass 2 (MEDIAN): bind 2=motion, 3=motionFiltered
    VkWriteDescriptorSet medianWrites[2] = {};
    medianWrites[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    medianWrites[0].dstSet          = descSets[1];
    medianWrites[0].dstBinding      = FG_BIND_MOTION;
    medianWrites[0].descriptorCount = 1;
    medianWrites[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    medianWrites[0].pImageInfo      = &motionImgInfo;

    medianWrites[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    medianWrites[1].dstSet          = descSets[1];
    medianWrites[1].dstBinding      = FG_BIND_OUTPUT;
    medianWrites[1].descriptorCount = 1;
    medianWrites[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    medianWrites[1].pImageInfo      = &mfiltImgInfo;

    s_fgVk.vkUpdateDescriptorSets(device, 2, medianWrites, 0, nullptr);

    // Pass 3 (WARP): bind 0=cur, 1=prev, 2=motionFiltered, 3=output
    VkWriteDescriptorSet warpWrites[4] = {};
    warpWrites[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    warpWrites[0].dstSet          = descSets[2];
    warpWrites[0].dstBinding      = FG_BIND_CURRENT;
    warpWrites[0].descriptorCount = 1;
    warpWrites[0].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    warpWrites[0].pImageInfo      = &curImgInfo;

    warpWrites[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    warpWrites[1].dstSet          = descSets[2];
    warpWrites[1].dstBinding      = FG_BIND_PREVIOUS;
    warpWrites[1].descriptorCount = 1;
    warpWrites[1].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    warpWrites[1].pImageInfo      = &prevImgInfo;

    warpWrites[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    warpWrites[2].dstSet          = descSets[2];
    warpWrites[2].dstBinding      = FG_BIND_MOTION;
    warpWrites[2].descriptorCount = 1;
    warpWrites[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    warpWrites[2].pImageInfo      = &mfiltImgInfo;

    warpWrites[3].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    warpWrites[3].dstSet          = descSets[2];
    warpWrites[3].dstBinding      = FG_BIND_OUTPUT;
    warpWrites[3].descriptorCount = 1;
    warpWrites[3].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    warpWrites[3].pImageInfo      = &outputImgInfo;

    s_fgVk.vkUpdateDescriptorSets(device, 4, warpWrites, 0, nullptr);

    // ----------------------------------------------------------------
    // Pass 1: Motion search
    // ----------------------------------------------------------------
    s_fgVk.vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineMotion);
    // TELEMETRY GATE (env vegas_telemetry=1, default OFF): the fill+bind
    // below is the whole per-dispatch diagnostic cost, so it is skipped
    // entirely when the gate is closed.  The buffer + set are still created
    // and always bound — the motion shader's atomicAdds need a valid target,
    // and an un-zeroed accumulating buffer is harmless when never read back.
    if (s_fgStatsEnabled) {
      // Zero stats buffer before this dispatch accumulates into it (the
      // previous dispatch's block-thread-0 atomicAdds must not carry over).
      s_fgVk.vkCmdFillBuffer(cmdBuf,
          reinterpret_cast<VkBuffer>(s_fgStatsBuffer),
          0, 4 * sizeof(uint32_t), 0);
      VkBufferMemoryBarrier statsBarrier = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
      statsBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      statsBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      statsBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      statsBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      statsBarrier.buffer        = reinterpret_cast<VkBuffer>(s_fgStatsBuffer);
      statsBarrier.offset        = 0;
      statsBarrier.size          = 4 * sizeof(uint32_t);
      s_fgVk.vkCmdPipelineBarrier(cmdBuf,
          VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          0, 0, nullptr, 1, &statsBarrier, 0, nullptr);
    }
    VkDescriptorSet motionSets[2] = {};
    motionSets[0] = descSets[0];
    motionSets[1] = reinterpret_cast<VkDescriptorSet>(s_fgStatsSet);
    s_fgVk.vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE,
        pipelineLayout, 0, 2, motionSets, 0, nullptr);
    uint32_t motionGX = (extent.width  + FG_TILE_SIZE - 1) / FG_TILE_SIZE;
    uint32_t motionGY = (extent.height + FG_TILE_SIZE - 1) / FG_TILE_SIZE;
    {
      // Shader contract: pc.xy = block count (int(pc.xy) compared to
      // gl_WorkGroupID). Must be the tile grid size, NOT a reciprocal —
      // int(1/16)=0 made every workgroup early-return.
      // pc.w = block luma variance threshold for the confidence gate:
      // blocks with variance below ~thr are textureless (sky/walls/fog)
      // → no meaningful motion match → distrust → warp presents current.
      // NOTE: must be > 0 — smoothstep(thr/2, thr*2, x) is undefined when
      // edge0 == edge1 (pc.w == 0 → driver-dependent garbage confidence).
      // pc.e = HUD graph rect in block coords (x0 > x1 = mask disabled);
      // masked blocks write {0,0,1,0} and skip the search entirely.
      float pcData[8] = {
        float(motionGX), float(motionGY),
        s_fgMotionPrevValid ? 1.0f : 0.0f, 0.0005f,
        float(hudX0), float(hudY0), float(hudX1), float(hudY1) };
      s_fgVk.vkCmdPushConstants(cmdBuf, pipelineLayout,
          VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcData), pcData);
    }

    s_fgVk.vkCmdDispatch(cmdBuf, motionGX, motionGY, 1);

    // Barrier: motionRaw GENERAL (write→read for median)
    VkImageMemoryBarrier motionBarrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    motionBarrier.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    motionBarrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    motionBarrier.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    motionBarrier.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    motionBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    motionBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    motionBarrier.image               = motionRaw;
    motionBarrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    s_fgVk.vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &motionBarrier);

    // ----------------------------------------------------------------
    // Pass 2: Median filter
    // ----------------------------------------------------------------
    s_fgVk.vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineMedian);
    s_fgVk.vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE,
        pipelineLayout, 0, 1, &descSets[1], 0, nullptr);
    // Median push constants: pc.xy = motion image size (int(pc.xy)
    // compared against pixelPos). Zeros made every workgroup early-return.
    {
      float pcData[4] = { float(motionGX), float(motionGY), 0.0f, 0.0f };
      s_fgVk.vkCmdPushConstants(cmdBuf, pipelineLayout,
          VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcData), pcData);
    }

    uint32_t medianGX = (motionGX + FG_MEDIAN_TILE - 1) / FG_MEDIAN_TILE;
    uint32_t medianGY = (motionGY + FG_MEDIAN_TILE - 1) / FG_MEDIAN_TILE;
    s_fgVk.vkCmdDispatch(cmdBuf, medianGX, medianGY, 1);

    // Barrier: motionFiltered GENERAL (write→read for warp)
    VkImageMemoryBarrier mfiltBarrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    mfiltBarrier.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    mfiltBarrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    mfiltBarrier.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    mfiltBarrier.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    mfiltBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mfiltBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mfiltBarrier.image               = motionFiltered;
    mfiltBarrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    s_fgVk.vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &mfiltBarrier);

    // ----------------------------------------------------------------
    // Pass 3: Warp + blend
    // ----------------------------------------------------------------
    s_fgVk.vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineWarp);
    s_fgVk.vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE,
        pipelineLayout, 0, 1, &descSets[2], 0, nullptr);
    {
      // Shader contract: pc[0].xy = output pixel size (int() bounds check),
      // pc[0].z = motion grid width (motionSize.x), motionSize.y = ceil(pc.y/16).
      // pc[1] = adaptive blend params (floor, slope, cap) — host policy:
      //   floor: 0.5 @ 12ms frame time -> 0.95 @ 33ms, linear, clamped.
      //          Slower frames mean the Motion pass is backed up and its
      //          vectors are untrustworthy — favor the current frame so a
      //          collapsed search cannot ghost the HUD.  Fast frames keep
      //          a low floor, preserving real interpolation.
      //   slope: 0.1 (ramp rate over motion magnitude).
      //   cap:   min(floor + 0.1, 0.95) — ramp is clamped to cap in-shader.
      //   pc[1].w: SAD confidence threshold (normalized block SAD ∈ [0,1];
      //   scene-cut distrust = clamp(sad / threshold, 0, 1); final blend =
      //   max(blendFromMag, cap * distrust).  A block whose best luma
      //   match is worse than the threshold is treated as a scene change
      //   and the warp presents mostly the current frame.
      //   Watchdog: 5 net slow waits (> 25ms) force floor 0.95; clean
      //             waits decay the counter by one (oscillation-tolerant).
      //   Input: gov.smoothFrameTimeMs — governor EMA, updated EVERY
      //          present in d3d11/d3d9/dxgi (1-frame lag), unlike
      //          s_lastFrameTime which only refreshes every 5th present.
      float blendFloor = 0.5f;
      if (s_fgSlowCount >= 5) {
        blendFloor = 0.95f;
      } else {
        float ftMs = s_smoothFrameTimeMs;
        if (ftMs > 12.0f) {
          blendFloor = 0.5f + 0.45f * (ftMs - 12.0f) / (33.0f - 12.0f);
          if (blendFloor > 0.95f) blendFloor = 0.95f;
        }
        if (blendFloor < 0.5f) blendFloor = 0.5f;
      }
      float blendSlope = 0.1f;
      float blendCap   = std::min(blendFloor + 0.1f, 0.95f);
      float sadThreshold = 0.15f;  // 15% mean luma error → full distrust

      float pcData[8] = {
        float(extent.width), float(extent.height),
        float(motionGX), 0.0f,
        blendFloor, blendSlope, blendCap, sadThreshold };
      s_fgVk.vkCmdPushConstants(cmdBuf, pipelineLayout,
          VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcData), pcData);
    }

    uint32_t warpGX = (extent.width  + FG_WARP_TILE - 1) / FG_WARP_TILE;
    uint32_t warpGY = (extent.height + FG_WARP_TILE - 1) / FG_WARP_TILE;
    s_fgVk.vkCmdDispatch(cmdBuf, warpGX, warpGY, 1);

    // ----------------------------------------------------------------
    // Post-dispatch Step 1: save curImage → prevDst (for next frame)
    // ----------------------------------------------------------------
    // curImage GENERAL → TRANSFER_SRC_OPTIMAL
    VkImageMemoryBarrier curToSrc = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    curToSrc.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    curToSrc.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    curToSrc.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    curToSrc.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    curToSrc.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    curToSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    curToSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    curToSrc.image               = actualCur;
    curToSrc.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // prevDst GENERAL → TRANSFER_DST_OPTIMAL
    VkImageMemoryBarrier prevToDst = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    prevToDst.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    prevToDst.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    prevToDst.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    prevToDst.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    prevToDst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    prevToDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    prevToDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    prevToDst.image               = prevDst;
    prevToDst.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkImageMemoryBarrier copyPrep[2] = { curToSrc, prevToDst };
    s_fgVk.vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 2, copyPrep);

    // Copy curImage → prevDst
    VkImageCopy copyCur = {};
    copyCur.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyCur.srcSubresource.layerCount = 1;
    copyCur.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyCur.dstSubresource.layerCount = 1;
    copyCur.extent = extent;

    s_fgVk.vkCmdCopyImage(cmdBuf,
        actualCur, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        prevDst,   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &copyCur);

    // ----------------------------------------------------------------
    // Post-dispatch Step 2: blit fgOutput → curImage (for presentation)
    // ----------------------------------------------------------------
    // curImage TRANSFER_SRC_OPTIMAL → TRANSFER_DST_OPTIMAL
    VkImageMemoryBarrier curToDst = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    curToDst.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    curToDst.srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    curToDst.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    curToDst.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    curToDst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    curToDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    curToDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    curToDst.image               = actualCur;
    curToDst.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // fgOutput GENERAL → TRANSFER_SRC_OPTIMAL (after shader write completes)
    VkImageMemoryBarrier fgToSrc = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    fgToSrc.sType                = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    fgToSrc.srcAccessMask        = VK_ACCESS_SHADER_WRITE_BIT;
    fgToSrc.dstAccessMask        = VK_ACCESS_TRANSFER_READ_BIT;
    fgToSrc.oldLayout            = VK_IMAGE_LAYOUT_GENERAL;
    fgToSrc.newLayout            = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    fgToSrc.srcQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
    fgToSrc.dstQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
    fgToSrc.image                = fgOutput;
    fgToSrc.subresourceRange     = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // Also transition prevDst: TRANSFER_DST → GENERAL for next frame read
    VkImageMemoryBarrier prevToGen = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    prevToGen.sType                = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    prevToGen.srcAccessMask        = VK_ACCESS_TRANSFER_WRITE_BIT;
    prevToGen.dstAccessMask        = VK_ACCESS_SHADER_READ_BIT;
    prevToGen.oldLayout            = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    prevToGen.newLayout            = VK_IMAGE_LAYOUT_GENERAL;
    prevToGen.srcQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
    prevToGen.dstQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
    prevToGen.image                = prevDst;
    prevToGen.subresourceRange     = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkImageMemoryBarrier blitPrep[3] = { curToDst, fgToSrc, prevToGen };
    s_fgVk.vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 3, blitPrep);

    // Blit fgOutput → curImage (nearest, 1:1)
    VkImageBlit blitFG = {};
    blitFG.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blitFG.srcSubresource.layerCount = 1;
    blitFG.srcOffsets[0] = { 0, 0, 0 };
    blitFG.srcOffsets[1] = { static_cast<int32_t>(extent.width),
                             static_cast<int32_t>(extent.height), 1 };
    blitFG.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blitFG.dstSubresource.layerCount = 1;
    blitFG.dstOffsets[0] = { 0, 0, 0 };
    blitFG.dstOffsets[1] = { static_cast<int32_t>(extent.width),
                             static_cast<int32_t>(extent.height), 1 };

    s_fgVk.vkCmdBlitImage(cmdBuf,
        fgOutput, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        actualCur, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &blitFG, VK_FILTER_NEAREST);

    // ----------------------------------------------------------------
    // Motion-prev refresh: copy motionFiltered → s_fgMotionPrevImage so
    // the NEXT frame's motion pass can center its search on the previous
    // frame's median-filtered vector (temporal prediction for pans).
    // ----------------------------------------------------------------
    VkImageMemoryBarrier prevMotionPrep[2] = {};
    prevMotionPrep[0].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    prevMotionPrep[0].srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;  // warp read
    prevMotionPrep[0].dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    prevMotionPrep[0].oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    prevMotionPrep[0].newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    prevMotionPrep[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    prevMotionPrep[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    prevMotionPrep[0].image               = motionFiltered;
    prevMotionPrep[0].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    prevMotionPrep[1].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    prevMotionPrep[1].srcAccessMask       = 0;
    prevMotionPrep[1].dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    prevMotionPrep[1].oldLayout           = s_fgMotionPrevValid
      ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    prevMotionPrep[1].newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    prevMotionPrep[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    prevMotionPrep[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    prevMotionPrep[1].image               = reinterpret_cast<VkImage>(s_fgMotionPrevImage);
    prevMotionPrep[1].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    s_fgVk.vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 2, prevMotionPrep);

    VkImageCopy prevMotionCopy = {};
    prevMotionCopy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    prevMotionCopy.srcSubresource.layerCount = 1;
    prevMotionCopy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    prevMotionCopy.dstSubresource.layerCount = 1;
    prevMotionCopy.extent = { s_fgMotionW, s_fgMotionH, 1 };

    s_fgVk.vkCmdCopyImage(cmdBuf,
        motionFiltered, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        reinterpret_cast<VkImage>(s_fgMotionPrevImage), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &prevMotionCopy);

    VkImageMemoryBarrier prevMotionDone[2] = {};
    prevMotionDone[0].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    prevMotionDone[0].srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    prevMotionDone[0].dstAccessMask       = 0;
    prevMotionDone[0].oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    prevMotionDone[0].newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    prevMotionDone[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    prevMotionDone[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    prevMotionDone[0].image               = motionFiltered;
    prevMotionDone[0].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    prevMotionDone[1].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    prevMotionDone[1].srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    prevMotionDone[1].dstAccessMask       = 0;
    prevMotionDone[1].oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    prevMotionDone[1].newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    prevMotionDone[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    prevMotionDone[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    prevMotionDone[1].image               = reinterpret_cast<VkImage>(s_fgMotionPrevImage);
    prevMotionDone[1].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    s_fgVk.vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 2, prevMotionDone);

    s_fgMotionPrevValid = true;

    // ----------------------------------------------------------------
    // Final barrier: restore curImage to PRESENT_SRC_KHR
    // ----------------------------------------------------------------
    VkImageMemoryBarrier curFinal = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    curFinal.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    curFinal.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    curFinal.dstAccessMask       = 0;
    curFinal.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    curFinal.newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    curFinal.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    curFinal.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    curFinal.image               = actualCur;
    curFinal.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    s_fgVk.vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &curFinal);

    // ----------------------------------------------------------------
    // End & submit
    // ----------------------------------------------------------------
    vr = s_fgVk.vkEndCommandBuffer(cmdBuf);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkEndCommandBuffer failed (", vr, ")"));
      fgCleanup(8); return false;
    }

    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &cmdBuf;

    vr = s_fgVk.vkQueueSubmit(queue, 1, &submitInfo, fence);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkQueueSubmit failed (", vr, ")"));
      fgCleanup(8); return false;
    }

    auto fgWaitStart = std::chrono::steady_clock::now();
    vr = s_fgVk.vkWaitForFences(device, 1, &fence, VK_TRUE, 50'000'000);
    // Watchdog input: actual time spent waiting (ms).  > 25ms counts as
    // a slow dispatch for the adaptive blend policy next frame.
    float fgWaitMs = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - fgWaitStart).count();
    if (vr != VK_SUCCESS) {
      // CB may still be executing — do NOT destroy here.  Park everything
      // in the pending list; a later dispatch drains it once the fence
      // signals.  This is the fix for the repeated-timeout hard freeze.
      Logger::warn(str::format("Vegas FG: vkWaitForFences timeout (", vr, ")"));
      VkImageView timedViews[6] = {
        srcViewCur, srcViewPrev, motionView, motionFilteredView, outputView,
        motionPrevView
      };
      fgQueuePending(device, fence, cmdPool, cmdBuf, timedViews, 6);
      if (fgWaitMs > 25.0f)
        s_fgSlowCount = std::min(s_fgSlowCount + 1u, 5u);
      return false;
    }
    // Success: slow wait increments, clean wait DECAYS by one (not a hard
    // reset) so oscillation between slow/fast frames cannot starve the
    // watchdog of its trip condition.
    if (fgWaitMs > 25.0f)
      s_fgSlowCount = std::min(s_fgSlowCount + 1u, 5u);
    else if (s_fgSlowCount > 0)
      s_fgSlowCount--;

    // Cleanup views
    s_fgVk.vkDestroyImageView(device, outputView, nullptr);
    s_fgVk.vkDestroyImageView(device, motionFilteredView, nullptr);
    s_fgVk.vkDestroyImageView(device, motionView, nullptr);
    s_fgVk.vkDestroyImageView(device, srcViewPrev, nullptr);
    s_fgVk.vkDestroyImageView(device, srcViewCur, nullptr);

    s_fgVk.vkDestroyFence(device, fence, nullptr);
    s_fgVk.vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
    s_fgVk.vkDestroyCommandPool(device, cmdPool, nullptr);

    // ---- Bimodal-gate diagnostic readback ----
    // Every dispatch (including timeouts that later drained) wrote its
    // own block statistics; log them so the 0.10/0.55 bound behavior is
    // observable in the device log instead of inferred from visuals.
    // GATED: only read back + log when env vegas_telemetry=1.  The buffer
    // is a single 16B block (words 0..3); the ring slots and the epoch
    // canary are gone — 2612/2612 rows were arithmetically consistent, so
    // the parked-CB premise was disproven and there is nothing to canary.
    if (s_fgStatsEnabled && s_fgStatsMapping) {
      const uint32_t* stats = reinterpret_cast<const uint32_t*>(s_fgStatsMapping);
      uint32_t count = stats[0];
      uint32_t sumQ  = stats[1];
      uint32_t zero  = stats[2];
      uint32_t full  = stats[3];
      int meanThousand = count ? int((double(sumQ) / double(count) / 65535.0) * 1000.0) : 0;
      uint32_t zeroPct = count ? zero * 100 / count : 0;
      uint32_t fullPct = count ? full * 100 / count : 0;
      // ---- Physical-bounds validation ----
      // A single clean dispatch cannot exceed this mean: zeroed blocks
      // contribute <= 0.10, mid blocks < 0.55, full blocks <= 1.0 (all
      // clamped in-shader).  Rows exceeding the bound are physically
      // impossible for one dispatch's atomics — if one ever appears it is
      // a genuine anomaly worth investigating, not a log-format artifact.
      double maxMean = 0.0;
      if (count > 0) {
        double zFrac = double(zero) / double(count);
        double fFrac = double(full) / double(count);
        double mFrac = (1.0 - zFrac - fFrac > 0.0) ? (1.0 - zFrac - fFrac) : 0.0;
        maxMean = zFrac * 0.10 + mFrac * 0.55 + fFrac * 1.0;
      }
      double mean = double(meanThousand) / 1000.0;
      if (mean > maxMean + 0.05) {
        s_fgStatsCorrupt++;
        Logger::debug(str::format(
          "Vegas FG: stat CORRUPT domMean=", meanThousand / 1000, ".", fmt03(uint32_t(meanThousand % 1000)),
          " blocks=", count, " zero<0.10=", zeroPct, "%",
          " full>=0.55=", fullPct, "%",
          " maxMean=", (int)(maxMean * 1000.0) / 1000, ".", fmt03((uint32_t)((int)(maxMean * 1000.0) % 1000)),
          " (total=", s_fgStatsCorrupt, ")"));
      } else {
        Logger::debug(str::format(
          "Vegas FG: stat domMean=", meanThousand / 1000, ".", fmt03(uint32_t(meanThousand % 1000)),
          " blocks=", count, " zero<0.10=", zeroPct, "%",
          " full>=0.55=", fullPct, "%"));
      }
      s_fgStatsFrames++;
  bool Framegen::isFgActive() {
    auto dev = s_dxvkDevice;
    if (dev != nullptr && dev->m_vegasMetrics.initialized)
      return dev->m_vegasMetrics.fgActive;
    return s_fgActive;
  }


