#pragma once

#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

#include "netbench/config.hpp"
#include "netbench/metrics.hpp"

namespace netbench {

struct SourceValidationReport {
  std::string actual_interface;
  std::string configured_source_ip;
  std::string destination_ip;
  std::string test_namespace;
  std::uint64_t probes_transmitted{};
  std::vector<std::string> local_errors;
  std::vector<std::string> response_observations;
  bool forwarding_drop_externally_verified{};
};

struct LabEnvironmentInfo {
  std::string actual_interface;
  std::string test_namespace;
};

class LabEnvironment {
public:
  [[nodiscard]] static LabEnvironmentInfo verify(const Config &config);
};

struct TestResult;

class SourceValidationEngine {
public:
  [[nodiscard]] static TestResult
  run(const Config &config, std::stop_token stop,
      const std::function<void(const MetricsSnapshot &)> &progress = {});
};

} // namespace netbench
