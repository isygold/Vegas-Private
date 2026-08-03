#include "dxvk_hud_item.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <version.h>
#include "../../util/log/log.h"

namespace dxvk::hud {

  HudItem::~HudItem() {

  }


  void HudItem::update(dxvk::high_resolution_clock::time_point time) {
    // Do nothing by default. Some items won't need this.
  }


  HudItemSet::HudItemSet(const Rc<DxvkDevice>& device) {
    std::string configStr = env::getEnvVar("DXVK_HUD");

    if (configStr.empty())
      configStr = device->config().hud;

    std::string::size_type pos = 0;
    std::string::size_type end = 0;
    std::string::size_type mid = 0;
    
    while (pos < configStr.size()) {
      end = configStr.find(',', pos);
      mid = configStr.find('=', pos);
      
      if (end == std::string::npos)
        end = configStr.size();
      
      if (mid != std::string::npos && mid < end) {
        m_options.insert({
          configStr.substr(pos,     mid - pos),
          configStr.substr(mid + 1, end - mid - 1) });
      } else {
        m_enabled.insert(configStr.substr(pos, end - pos));
      }

      pos = end + 1;
    }

    if (m_enabled.find("full") != m_enabled.end())
      m_enableFull = true;
    
    if (m_enabled.find("1") != m_enabled.end()) {
      m_enabled.insert("devinfo");
      m_enabled.insert("fps");
    }

    // DEBUG: resolved HUD config source + enabled token set (temp)
    Logger::info(str::format("HUD debug: env DXVK_HUD=\"", env::getEnvVar("DXVK_HUD"), "\""));
    Logger::info(str::format("HUD debug: resolved configStr=\"", configStr, "\""));
    for (const auto& tok : m_enabled)
      Logger::info(str::format("HUD debug: enabled=\"", tok, "\""));
  }


  HudItemSet::~HudItemSet() {

  }


  void HudItemSet::update() {
    auto time = dxvk::high_resolution_clock::now();

    for (const auto& item : m_items)
      item->update(time);
  }


  void HudItemSet::render(HudRenderer& renderer) {
    HudPos position = { 8.0f, 8.0f };

    for (const auto& item : m_items)
      position = item->render(renderer, position);
  }


  void HudItemSet::parseOption(const std::string& str, float& value) {
    try {
      value = std::stof(str);
    } catch (const std::invalid_argument&) {
      return;
    }
  }


  HudPos HudVersionItem::render(
          HudRenderer&      renderer,
          HudPos            position) {
    position.y += 16.0f;

    renderer.drawText(16.0f,
      { position.x, position.y },
      { 1.0f, 1.0f, 1.0f, 1.0f },
      "VEGAS " DXVK_RELEASE);

    position.y += 8.0f;
    return position;
  }


  HudPos HudCommitItem::render(
          HudRenderer&      renderer,
          HudPos            position) {
    position.y += 16.0f;

    // Short commit identity only.  git-describe tags carry "-g<hash>"
    // ("2.4.1-11-g8d2f1f7" -> "-g8d2f1f7"); GHA builds are a raw short
    // hash ("f48d326") with no "-g" — printed as-is.
    std::string commit = DXVK_VERSION;
    const size_t pos = commit.rfind("-g");
    if (pos != std::string::npos)
      commit = commit.substr(pos);

    renderer.drawText(16.0f,
      { position.x, position.y },
      { 1.0f, 1.0f, 1.0f, 1.0f },
      str::format("Commit: ", commit));

    position.y += 8.0f;
    return position;
  }


  // Expand a kernel cpu list ("0-3,8-11", "0,2,4", "0") to core ids in
  // ascending order.  Malformed tokens and inverted ranges are skipped.
  static std::vector<uint32_t> expandCpuList(const std::string& list) {
    std::vector<uint32_t> ids;
    size_t pos = 0;
    while (pos < list.size()) {
      size_t comma = list.find(',', pos);
      std::string token = list.substr(pos,
        comma == std::string::npos ? std::string::npos : comma - pos);
      size_t dash = token.find('-');
      if (dash == std::string::npos) {
        uint32_t id = std::atoi(token.c_str());
        if (id < HudCpuItem::CpuMaxCores)
          ids.push_back(id);
      } else {
        uint32_t lo = std::atoi(token.substr(0, dash).c_str());
        uint32_t hi = std::atoi(token.substr(dash + 1).c_str());
        if (hi >= lo) {
          for (uint32_t id = lo; id <= hi; id++) {
            if (id < HudCpuItem::CpuMaxCores)
              ids.push_back(id);
          }
        }
      }
      if (comma == std::string::npos)
        break;
      pos = comma + 1;
    }
    return ids;
  }


  HudCpuItem::HudCpuItem() {
    // Far-past timestamp forces an immediate first sample; the first
    // deltas are then available one interval later (no "--" stall).
    m_lastSample = std::chrono::steady_clock::time_point::min();
  }


  void HudCpuItem::sample() {
    const auto now = std::chrono::steady_clock::now();
    if (now - m_lastSample < std::chrono::milliseconds(1000))
      return;
    m_lastSample = now;

    // Active core ids from the kernel hotplug mask.
    std::ifstream onlineFile("/sys/devices/system/cpu/online");
    std::string onlineStr;
    if (onlineFile)
      std::getline(onlineFile, onlineStr);
    m_onlineIds = expandCpuList(onlineStr);

    // Cores that left the online set lose their delta base so a hotplug
    // return cannot produce a stale-range load.
    for (uint32_t i = 0; i < HudCpuItem::CpuMaxCores; i++) {
      bool online = false;
      for (uint32_t id : m_onlineIds) {
        if (id == i) { online = true; break; }
      }
      if (!online)
        m_prevValid[i] = false;
    }

    // Per-core loads from /proc/stat.  The aggregate line ("cpu ") is
    // skipped; busy excludes idle + iowait so I/O waits do not inflate
    // the reported compute load.
    uint32_t pctById[HudCpuItem::CpuMaxCores]  = {};
    bool     validById[HudCpuItem::CpuMaxCores] = {};
    std::ifstream statFile("/proc/stat");
    if (statFile) {
      std::string line;
      while (std::getline(statFile, line)) {
        if (line.rfind("cpu", 0) != 0)
          break;                     // non-cpu lines follow the cpu block
        if (line.size() <= 3 || line[3] == ' ')
          continue;                  // aggregate "cpu  ..." line
        uint32_t id = std::atoi(line.substr(3).c_str());
        if (id >= HudCpuItem::CpuMaxCores)
          continue;
        uint64_t fields[7] = { 0, 0, 0, 0, 0, 0, 0 };
        std::istringstream ss(line.substr(3));
        std::string idStr;
        ss >> idStr;
        int fieldIdx = 0;
        while (fieldIdx < 7 && (ss >> fields[fieldIdx]))
          fieldIdx++;
        // user nice system idle iowait irq softirq
        uint64_t idle  = fields[3] + fields[4];
        uint64_t total = fields[0] + fields[1] + fields[2] + fields[3]
                       + fields[4] + fields[5] + fields[6];
        if (m_prevValid[id] && total >= m_prevTotal[id] && idle >= m_prevIdle[id]) {
          uint64_t dIdle  = idle - m_prevIdle[id];
          uint64_t dTotal = total - m_prevTotal[id];
          if (dTotal > 0) {
            pctById[id]  = uint32_t((dTotal - dIdle) * 100 / dTotal);
            validById[id] = true;
          }
        }
        m_prevIdle[id]  = idle;
        m_prevTotal[id] = total;
        m_prevValid[id] = true;
      }
    }

    // Build display rows, up to 4 cores per row to keep the line width
    // within the HUD text area on phone screens.
    m_rows.clear();
    std::string row = "Cpu:";
    uint32_t inRow = 0;
    for (uint32_t id : m_onlineIds) {
      std::string entry = str::format(" C", id, " ",
        validById[id] ? std::to_string(pctById[id]) : "--", "%");
      if (inRow >= 4) {
        m_rows.push_back(row);
        row.clear();
        inRow = 0;
      }
      row += entry;
      inRow++;
    }
    if (row == "Cpu:" && m_rows.empty())
      row += " --";                  // no core mask readable
    m_rows.push_back(row);

    // DEBUG: cpu HUD sample result (temp)
    Logger::info(str::format("HUD debug: cpu rows=", m_rows.size()));
    if (!m_rows.empty())
      Logger::info(str::format("HUD debug: cpu row0=\"", m_rows.front(), "\""));
  }


  HudPos HudCpuItem::render(
          HudRenderer&      renderer,
          HudPos            position) {
    sample();

    position.y += 16.0f;

    for (const std::string& row : m_rows) {
      renderer.drawText(16.0f,
        { position.x, position.y },
        { 1.0f, 1.0f, 1.0f, 1.0f },
        row);
      position.y += 8.0f;
    }
    return position;
  }


  HudClientApiItem::HudClientApiItem(std::string api)
  : m_api(api) {

  }


  HudClientApiItem::~HudClientApiItem() {

  }


  HudPos HudClientApiItem::render(
          HudRenderer&      renderer,
          HudPos            position) {
    position.y += 16.0f;

    renderer.drawText(16.0f,
      { position.x, position.y },
      { 1.0f, 1.0f, 1.0f, 1.0f },
      m_api);

    position.y += 8.0f;
    return position;
  }


  HudDeviceInfoItem::HudDeviceInfoItem(const Rc<DxvkDevice>& device) {
    const auto& props = device->properties();

    std::string driverInfo = props.vk12.driverInfo;

    if (driverInfo.empty())
      driverInfo = props.driverVersion.toString();

    m_deviceName = props.core.properties.deviceName;
    m_driverName = str::format("Driver:  ", props.vk12.driverName);
    m_driverVer = str::format("Version: ", driverInfo);
  }


  HudDeviceInfoItem::~HudDeviceInfoItem() {

  }


  HudPos HudDeviceInfoItem::render(
          HudRenderer&      renderer,
          HudPos            position) {
    position.y += 16.0f;
    renderer.drawText(16.0f,
      { position.x, position.y },
      { 1.0f, 1.0f, 1.0f, 1.0f },
      m_deviceName);
    
    position.y += 24.0f;
    renderer.drawText(16.0f,
      { position.x, position.y },
      { 1.0f, 1.0f, 1.0f, 1.0f },
      m_driverName);
    
    position.y += 20.0f;
    renderer.drawText(16.0f,
      { position.x, position.y },
      { 1.0f, 1.0f, 1.0f, 1.0f },
      m_driverVer);

    position.y += 8.0f;
    return position;
  }


  HudFpsItem::HudFpsItem() { }
  HudFpsItem::~HudFpsItem() { }


  void HudFpsItem::update(dxvk::high_resolution_clock::time_point time) {
    m_frameCount += 1;

    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(time - m_lastUpdate);

    if (elapsed.count() >= UpdateInterval) {
      int64_t fps = (10'000'000ll * m_frameCount) / elapsed.count();

      m_frameRate = str::format(fps / 10, ".", fps % 10);
      m_frameCount = 0;
      m_lastUpdate = time;
    }
  }


  HudPos HudFpsItem::render(
          HudRenderer&      renderer,
          HudPos            position) {
    position.y += 16.0f;

    renderer.drawText(16.0f,
      { position.x, position.y },
      { 1.0f, 0.25f, 0.25f, 1.0f },
      "FPS:");

    renderer.drawText(16.0f,
      { position.x + 60.0f, position.y },
      { 1.0f, 1.0f, 1.0f, 1.0f },
      m_frameRate);

    position.y += 8.0f;
    return position;
  }


  HudFrameTimeItem::HudFrameTimeItem() { }
  HudFrameTimeItem::~HudFrameTimeItem() { }


  void HudFrameTimeItem::update(dxvk::high_resolution_clock::time_point time) {
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(time - m_lastUpdate);

    m_dataPoints[m_dataPointId] = float(elapsed.count());
    m_dataPointId = (m_dataPointId + 1) % NumDataPoints;

    m_lastUpdate = time;
  }


  HudPos HudFrameTimeItem::render(
          HudRenderer&      renderer,
          HudPos            position) {
    std::array<HudGraphPoint, NumDataPoints> points;

    // 60 FPS = optimal, 10 FPS = worst
    const float targetUs =  16'666.6f;
    const float minUs    =   5'000.0f;
    const float maxUs    = 100'000.0f;
    
    // Ten times the maximum/minimum number
    // of milliseconds for a single frame
    uint32_t minMs = 0xFFFFFFFFu;
    uint32_t maxMs = 0x00000000u;
    
    // Paint the time points
    for (uint32_t i = 0; i < NumDataPoints; i++) {
      float us = m_dataPoints[(m_dataPointId + i) % NumDataPoints];
      
      minMs = std::min(minMs, uint32_t(us / 100.0f));
      maxMs = std::max(maxMs, uint32_t(us / 100.0f));
      
      float r = std::min(std::max(-1.0f + us / targetUs, 0.0f), 1.0f);
      float g = std::min(std::max( 3.0f - us / targetUs, 0.0f), 1.0f);
      float l = std::sqrt(r * r + g * g);
      
      HudNormColor color = {
        uint8_t(255.0f * (r / l)),
        uint8_t(255.0f * (g / l)),
        uint8_t(0), uint8_t(255) };
      
      float hVal = std::log2(std::max((us - minUs) / targetUs + 1.0f, 1.0f))
                 / std::log2((maxUs - minUs) / targetUs);
      
      points[i].value = std::max(hVal, 1.0f / 40.0f);
      points[i].color = color;
    }
    
    renderer.drawGraph(position,
      HudPos { float(NumDataPoints), 40.0f },
      points.size(), points.data());
    
    position.y += 58.0f;

    renderer.drawText(12.0f,
      { position.x, position.y },
      { 1.0f, 0.25f, 0.25f, 1.0f },
      "min:");

    renderer.drawText(12.0f,
      { position.x + 45.0f, position.y },
      { 1.0f, 1.0f, 1.0f, 1.0f },
      str::format(minMs / 10, ".", minMs % 10));
    
    renderer.drawText(12.0f,
      { position.x + 150.0f, position.y },
      { 1.0f, 0.25f, 0.25f, 1.0f },
      "max:");
    
    renderer.drawText(12.0f,
      { position.x + 195.0f, position.y },
      { 1.0f, 1.0f, 1.0f, 1.0f },
      str::format(maxMs / 10, ".", maxMs % 10));
    
    position.y += 4.0f;
    return position;
  }


  HudSubmissionStatsItem::HudSubmissionStatsItem(const Rc<DxvkDevice>& device)
  : m_device(device) {

  }


  HudSubmissionStatsItem::~HudSubmissionStatsItem() {

  }


  void HudSubmissionStatsItem::update(dxvk::high_resolution_clock::time_point time) {
    DxvkStatCounters counters = m_device->getStatCounters();
    
    uint64_t currSubmitCount = counters.getCtr(DxvkStatCounter::QueueSubmitCount);
    uint64_t currSyncCount = counters.getCtr(DxvkStatCounter::GpuSyncCount);
    uint64_t currSyncTicks = counters.getCtr(DxvkStatCounter::GpuSyncTicks);

    m_maxSubmitCount = std::max(m_maxSubmitCount, currSubmitCount - m_prevSubmitCount);
    m_maxSyncCount = std::max(m_maxSyncCount, currSyncCount - m_prevSyncCount);
    m_maxSyncTicks = std::max(m_maxSyncTicks, currSyncTicks - m_prevSyncTicks);

    m_prevSubmitCount = currSubmitCount;
    m_prevSyncCount = currSyncCount;
    m_prevSyncTicks = currSyncTicks;

    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(time - m_lastUpdate);

    if (elapsed.count() >= UpdateInterval) {
      m_submitString = str::format(m_maxSubmitCount);

      uint64_t syncTicks = m_maxSyncTicks / 100;

      m_syncString = m_maxSyncCount
        ? str::format(m_maxSyncCount, " (", (syncTicks / 10), ".", (syncTicks % 10), " ms)")
        : str::format(m_maxSyncCount);

      m_maxSubmitCount = 0;
      m_maxSyncCount = 0;
      m_maxSyncTicks = 0;

      m_lastUpdate = time;
    }
  }


  HudPos HudSubmissionStatsItem::render(
          HudRenderer&      renderer,
          HudPos            position) {
    position.y += 16.0f;

    renderer.drawText(16.0f,
      { position.x, position.y },
      { 1.0f, 0.5f, 0.25f, 1.0f },
      "Queue submissions:");

    renderer.drawText(16.0f,
      { position.x + 228.0f, position.y },
      { 1.0f, 1.0f, 1.0f, 1.0f },
      m_submitString);

    position.y += 20.0f;
    renderer.drawText(16.0f,
      { position.x, position.y },
      { 1.0f, 0.5f, 0.25f, 1.0f },
      "Queue syncs:");

    renderer.drawText(16.0f,
      { position.x + 228.0f, position.y },
      { 1.0f, 1.0f, 1.0f, 1.0f },
      m_syncString);

    position.y += 8.0f;
    return position;
  }


  HudDrawCallStatsItem::HudDrawCallStatsItem(const Rc<DxvkDevice>& device)
  : m_device(device) {

  }


  HudDrawCallStatsItem::~HudDrawCallStatsItem() {

  }


  void HudDrawCallStatsItem::update(dxvk::high_resolution_clock::time_point time) {
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(time - m_lastUpdate);

    DxvkStatCounters counters = m_device->getStatCounters();
    auto diffCounters = counters.diff(m_prevCounters);

    if (elapsed.count() >= UpdateInterval) {
      m_gpCount = diffCounters.getCtr(DxvkStatCounter::CmdDrawCalls);
      m_cpCount = diffCounters.getCtr(DxvkStatCounter::CmdDispatchCalls);
      m_rpCount = diffCounters.getCtr(DxvkStatCounter::CmdRenderPassCount);
      m_pbCount = diffCounters.getCtr(DxvkStatCounter::CmdBarrierCount);

      m_lastUpdate = time;
    }

    m_prevCounters = counters;
  }


  HudPos HudDrawCallStatsItem::render(
          HudRenderer&      renderer,
          HudPos            position) {
    position.y += 16.0f;
    renderer.drawText(16.0f,
      { position.x, position.y },
      { 0.25f, 0.5f, 1.0f, 1.0f },
      "Draw calls:");
    
    renderer.drawText(16.0f,
      { position.x + 192.0f, position.y },
      { 1.0f, 1.0f, 1.0f, 1.0f },
      str::format(m_gpCount));
    
    position.y += 20.0f;
    renderer.drawText(16.0f,
      { position.x, position.y },
      { 0.25f, 0.5f, 1.0f, 1.0f },
      "Dispatch calls:");
    
    renderer.drawText(16.0f,
      { position.x + 192.0f, position.y },
      { 1.0f, 1.0f, 1.0f, 1.0f },
      str::format(m_cpCount));
    
    position.y += 20.0f;
    renderer.drawText(16.0f,
      { position.x, position.y },
      { 0.25f, 0.5f, 1.0f, 1.0f },
      "Render passes:");
    
    renderer.drawText(16.0f,
      { position.x + 192.0f, position.y },
      { 1.0f, 1.0f, 1.0f, 1.0f },
      str::format(m_rpCount));
    
    position.y += 20.0f;
    renderer.drawText(16.0f,
      { position.x, position.y },
      { 0.25f, 0.5f, 1.0f, 1.0f },
      "Barriers:");
    
    renderer.drawText(16.0f,
      { position.x + 192.0f, position.y },
      { 1.0f, 1.0f, 1.0f, 1.0f },
      str::format(m_pbCount));
    
    position.y += 8.0f;
    return position;
  }


  HudPipelineStatsItem::HudPipelineStatsItem(const Rc<DxvkDevice>& device)
  : m_device(device) {

  }


  HudPipelineStatsItem::~HudPipelineStatsItem() {

  }


  void HudPipelineStatsItem::update(dxvk::high_resolution_clock::time_point time) {
    DxvkStatCounters counters = m_device->getStatCounters();

    m_graphicsPipelines = counters.getCtr(DxvkStatCounter::PipeCountGraphics);
    m_graphicsLibraries = counters.getCtr(DxvkStatCounter::PipeCountLibrary);
    m_computePipelines  = counters.getCtr(DxvkStatCounter::PipeCountCompute);
  }


  HudPos HudPipelineStatsItem::render(
          HudRenderer&      renderer,
          HudPos            position) {
    position.y += 16.0f;
    renderer.drawText(16.0f,
      { position.x, position.y },
      { 1.0f, 0.25f, 1.0f, 1.0f },
      "Graphics pipelines:");
    
    renderer.drawText(16.0f,
      { position.x + 240.0f, position.y },
      { 1.0f, 1.0f, 1.0f, 1.0f },
      str::format(m_graphicsPipelines));

    if (m_graphicsLibraries) {
      position.y += 20.0f;
      renderer.drawText(16.0f,
        { position.x, position.y },
        { 1.0f, 0.25f, 1.0f, 1.0f },
        "Graphics shaders:");

      renderer.drawText(16.0f,
        { position.x + 240.0f, position.y },
        { 1.0f, 1.0f, 1.0f, 1.0f },
        str::format(m_graphicsLibraries));
    }

    position.y += 20.0f;
    renderer.drawText(16.0f,
      { position.x, position.y },
      { 1.0f, 0.25f, 1.0f, 1.0f },
      "Compute pipelines:");

    renderer.drawText(16.0f,
      { position.x + 240.0f, position.y },
      { 1.0f, 1.0f, 1.0f, 1.0f },
      str::format(m_computePipelines));

    position.y += 8.0f;
    return position;
  }


  HudDescriptorStatsItem::HudDescriptorStatsItem(const Rc<DxvkDevice>& device)
  : m_device(device) {

  }


  HudDescriptorStatsItem::~HudDescriptorStatsItem() {

  }


  void HudDescriptorStatsItem::update(dxvk::high_resolution_clock::time_point time) {
    DxvkStatCounters counters = m_device->getStatCounters();

    m_descriptorPoolCount = counters.getCtr(DxvkStatCounter::DescriptorPoolCount);
    m_descriptorSetCount  = counters.getCtr(DxvkStatCounter::DescriptorSetCount);
  }


  HudPos HudDescriptorStatsItem::render(
          HudRenderer&      renderer,
          HudPos            position) {
    position.y += 16.0f;
    renderer.drawText(16.0f,
      { position.x, position.y },
      { 1.0f, 0.25f, 0.5f, 1.0f },
      "Descriptor pools:");
    
    renderer.drawText(16.0f,
      { position.x + 216.0f, position.y },
      { 1.0f, 1.0f, 1.0f, 1.0f },
      str::format(m_descriptorPoolCount));
    
    position.y += 20.0f;
    renderer.drawText(16.0f,
      { position.x, position.y },
      { 1.0f, 0.25f, 0.5f, 1.0f },
      "Descriptor sets:");

    renderer.drawText(16.0f,
      { position.x + 216.0f, position.y },
      { 1.0f, 1.0f, 1.0f, 1.0f },
      str::format(m_descriptorSetCount));

    position.y += 8.0f;
    return position;
  }


  HudMemoryStatsItem::HudMemoryStatsItem(const Rc<DxvkDevice>& device)
  : m_device(device), m_memory(device->adapter()->memoryProperties()) {

  }


  HudMemoryStatsItem::~HudMemoryStatsItem() {

  }


  void HudMemoryStatsItem::update(dxvk::high_resolution_clock::time_point time) {
    for (uint32_t i = 0; i < m_memory.memoryHeapCount; i++)
      m_heaps[i] = m_device->getMemoryStats(i);
  }


  HudPos HudMemoryStatsItem::render(
          HudRenderer&      renderer,
          HudPos            position) {
    for (uint32_t i = 0; i < m_memory.memoryHeapCount; i++) {
      bool isDeviceLocal = m_memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;

      uint64_t memUsedMib = m_heaps[i].memoryUsed >> 20;
      uint64_t memAllocatedMib = m_heaps[i].memoryAllocated >> 20;
      uint64_t percentage = (100 * m_heaps[i].memoryAllocated) / m_memory.memoryHeaps[i].size;

      std::string label = str::format(isDeviceLocal ? "Vidmem" : "Sysmem", " heap ", i, ": ");
      std::string text  = str::format(std::setfill(' '), std::setw(5), memAllocatedMib, " MB (", percentage, "%) ",
        std::setw(5 + (percentage < 10 ? 1 : 0) + (percentage < 100 ? 1 : 0)), memUsedMib, " MB used");

      position.y += 16.0f;
      renderer.drawText(16.0f,
        { position.x, position.y },
        { 1.0f, 1.0f, 0.25f, 1.0f },
        label);

      renderer.drawText(16.0f,
        { position.x + 168.0f, position.y },
        { 1.0f, 1.0f, 1.0f, 1.0f },
        text);
      position.y += 4.0f;
    }

    position.y += 4.0f;
    return position;
  }


  HudCsThreadItem::HudCsThreadItem(const Rc<DxvkDevice>& device)
  : m_device(device) {

  }


  HudCsThreadItem::~HudCsThreadItem() {

  }


  void HudCsThreadItem::update(dxvk::high_resolution_clock::time_point time) {
    uint64_t ticks = std::chrono::duration_cast<std::chrono::microseconds>(time - m_lastUpdate).count();

    // Capture the maximum here since it's more useful to
    // identify stutters than using any sort of average
    DxvkStatCounters counters = m_device->getStatCounters();
    uint64_t currCsSyncCount = counters.getCtr(DxvkStatCounter::CsSyncCount);
    uint64_t currCsSyncTicks = counters.getCtr(DxvkStatCounter::CsSyncTicks);

    m_maxCsSyncCount = std::max(m_maxCsSyncCount, currCsSyncCount - m_prevCsSyncCount);
    m_maxCsSyncTicks = std::max(m_maxCsSyncTicks, currCsSyncTicks - m_prevCsSyncTicks);

    m_prevCsSyncCount = currCsSyncCount;
    m_prevCsSyncTicks = currCsSyncTicks;

    m_updateCount++;

    if (ticks >= UpdateInterval) {
      uint64_t currCsChunks = counters.getCtr(DxvkStatCounter::CsChunkCount);
      uint64_t diffCsChunks = (currCsChunks - m_prevCsChunks) / m_updateCount;
      m_prevCsChunks = currCsChunks;

      uint64_t syncTicks = m_maxCsSyncTicks / 100;

      m_csChunkString = str::format(diffCsChunks);
      m_csSyncString = m_maxCsSyncCount
        ? str::format(m_maxCsSyncCount, " (", (syncTicks / 10), ".", (syncTicks % 10), " ms)")
        : str::format(m_maxCsSyncCount);

      m_maxCsSyncCount = 0;
      m_maxCsSyncTicks = 0;

      m_updateCount = 0;
      m_lastUpdate = time;
    }
  }


  HudPos HudCsThreadItem::render(
          HudRenderer&      renderer,
          HudPos            position) {
    position.y += 16.0f;
    renderer.drawText(16.0f,
      { position.x, position.y },
      { 0.25f, 1.0f, 0.25f, 1.0f },
      "CS chunks:");

    renderer.drawText(16.0f,
      { position.x + 132.0f, position.y },
      { 1.0f, 1.0f, 1.0f, 1.0f },
      m_csChunkString);

    position.y += 20.0f;
    renderer.drawText(16.0f,
      { position.x, position.y },
      { 0.25f, 1.0f, 0.25f, 1.0f },
      "CS syncs:");

    renderer.drawText(16.0f,
      { position.x + 132.0f, position.y },
      { 1.0f, 1.0f, 1.0f, 1.0f },
      m_csSyncString);

    position.y += 8.0f;
    return position;
  }


  HudGpuLoadItem::HudGpuLoadItem(const Rc<DxvkDevice>& device)
  : m_device(device) {

  }


  HudGpuLoadItem::~HudGpuLoadItem() {

  }


  void HudGpuLoadItem::update(dxvk::high_resolution_clock::time_point time) {
    uint64_t ticks = std::chrono::duration_cast<std::chrono::microseconds>(time - m_lastUpdate).count();

    if (ticks >= UpdateInterval) {
      DxvkStatCounters counters = m_device->getStatCounters();
      uint64_t currGpuIdleTicks = counters.getCtr(DxvkStatCounter::GpuIdleTicks);

      m_diffGpuIdleTicks = currGpuIdleTicks - m_prevGpuIdleTicks;
      m_prevGpuIdleTicks = currGpuIdleTicks;

      uint64_t busyTicks = ticks > m_diffGpuIdleTicks
        ? uint64_t(ticks - m_diffGpuIdleTicks)
        : uint64_t(0);

      m_gpuLoadString = str::format((100 * busyTicks) / ticks, "%");
      m_lastUpdate = time;
    }
  }


  HudPos HudGpuLoadItem::render(
          HudRenderer&      renderer,
          HudPos            position) {
    position.y += 16.0f;

    renderer.drawText(16.0f,
      { position.x, position.y },
      { 0.25f, 0.5f, 0.25f, 1.0f },
      "GPU:");

    renderer.drawText(16.0f,
      { position.x + 60.0f, position.y },
      { 1.0f, 1.0f, 1.0f, 1.0f },
      m_gpuLoadString);

    position.y += 8.0f;
    return position;
  }


  HudCompilerActivityItem::HudCompilerActivityItem(const Rc<DxvkDevice>& device)
  : m_device(device) {

  }


  HudCompilerActivityItem::~HudCompilerActivityItem() {

  }


  void HudCompilerActivityItem::update(dxvk::high_resolution_clock::time_point time) {
    DxvkStatCounters counters = m_device->getStatCounters();

    m_tasksDone = counters.getCtr(DxvkStatCounter::PipeTasksDone);
    m_tasksTotal = counters.getCtr(DxvkStatCounter::PipeTasksTotal);

    bool doShow = m_tasksDone < m_tasksTotal;

    if (!doShow)
      m_timeDone = time;

    if (!m_show) {
      m_timeShown = time;
      m_showPercentage = false;
    } else {
      auto durationShown = std::chrono::duration_cast<std::chrono::milliseconds>(time - m_timeShown);
      auto durationWorking = std::chrono::duration_cast<std::chrono::milliseconds>(time - m_timeDone);

      if (!doShow) {
        m_offset = m_tasksTotal;

        // Ensure the item stays up long enough to be legible
        doShow = durationShown.count() <= MinShowDuration;
      }

      if (!m_showPercentage) {
        // Don't show percentage if it's just going to be stuck at 99%
        // because the workers are not being fed tasks fast enough
        m_showPercentage = durationWorking.count() >= (MinShowDuration / 5)
                        && (computePercentage() < 50);
      }
    }

    m_show = doShow;
  }


  HudPos HudCompilerActivityItem::render(
          HudRenderer&      renderer,
          HudPos            position) {
    if (m_show) {
      std::string string = "Compiling shaders...";

      if (m_showPercentage)
        string = str::format(string, " (", computePercentage(), "%)");

      renderer.drawText(16.0f,
        { position.x, renderer.surfaceSize().height / renderer.scale() - 20.0f },
        { 1.0f, 1.0f, 1.0f, 1.0f },
        string);
    }

    return position;
  }


  uint32_t HudCompilerActivityItem::computePercentage() const {
    if (m_offset == m_tasksTotal)
      return 100;

    return (uint32_t(m_tasksDone - m_offset) * 100)
         / (uint32_t(m_tasksTotal - m_offset));
  }


  HudVegasItem::HudVegasItem() {

  }


  HudVegasItem::~HudVegasItem() {

  }


  void HudVegasItem::update(dxvk::high_resolution_clock::time_point time) {
    const auto& gov = Vegas::s_gov;

    m_vegasString = str::format(
      "Vegas: GPU=", uint32_t(gov.realGpuLoadEMA * 100.0f), "%",
      " cap=", gov.dynamicMaxBatchCap,
      " thr=", gov.drawThreshold,
      " flush=", gov.targetFlushesPerFrame,
      " draw=", uint32_t(gov.drawLoadEMA));
  }


  HudPos HudVegasItem::render(
          HudRenderer&      renderer,
          HudPos            position) {
    position.y += 16.0f;

    renderer.drawText(16.0f,
      { position.x, position.y },
      { 0.25f, 0.50f, 1.0f, 1.0f },
      m_vegasString);

    position.y += 8.0f;
    return position;
  }

}
