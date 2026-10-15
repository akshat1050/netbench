#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>

#include "netbench/config.hpp"

namespace netbench {

class SourcePolicy {
public:
  static constexpr std::uint64_t maximum_pps = 10;
  static constexpr auto maximum_duration = std::chrono::seconds{30};
  static constexpr std::uint64_t maximum_probes = 300;

  static void validate(const Config &config);
  [[nodiscard]] static bool is_rfc1918(std::string_view address);
};

} // namespace netbench
