#pragma once

#include <functional>
#include <optional>
#include <stop_token>
#include <string_view>

#include "netbench/config.hpp"
#include "netbench/metrics.hpp"

namespace netbench {

struct TestResult {
  std::string_view protocol;
  MetricsSnapshot metrics;
  std::optional<LatencySummary> latency;
  std::optional<LatencySummary> before_responsiveness;
  std::optional<LatencySummary> after_responsiveness;
  std::optional<double> packet_loss_percent;
  ResourceUsage resources;
};

using ProgressCallback = std::function<void(const MetricsSnapshot &)>;

[[nodiscard]] TestResult run_tcp_test(const Config &config,
                                      std::stop_token stop,
                                      const ProgressCallback &progress = {});
[[nodiscard]] TestResult run_udp_test(const Config &config,
                                      std::stop_token stop,
                                      const ProgressCallback &progress = {});
[[nodiscard]] TestResult run_icmp_test(const Config &config,
                                       std::stop_token stop,
                                       const ProgressCallback &progress = {});

} // namespace netbench
