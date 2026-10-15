#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace netbench {

enum class Command { tcp, udp, icmp, validate_source, help, version };
enum class Profile { baseline, sustained, ramp, burst };

struct SafetyLimits {
  static constexpr std::uint64_t absolute_pps = 1'000'000;
  static constexpr std::uint64_t absolute_bandwidth_bytes_per_second =
      1'250'000'000ULL;
  static constexpr std::uint64_t absolute_connection_rate = 10'000;
  static constexpr std::uint32_t absolute_concurrency = 4096;
  static constexpr auto absolute_duration = std::chrono::seconds{300};

  std::uint64_t pps{100'000};
  std::uint64_t bandwidth_bytes_per_second{125'000'000};
  std::uint64_t connection_rate{1'000};
  std::uint32_t concurrency{1024};
  std::chrono::milliseconds duration{std::chrono::seconds{60}};
};

class CidrV4 {
public:
  static CidrV4 parse(std::string_view text);
  [[nodiscard]] bool contains(std::string_view address) const;
  [[nodiscard]] bool contains_usable_host(std::string_view address) const;
  [[nodiscard]] bool is_subset_of(const CidrV4 &other) const noexcept;
  [[nodiscard]] const std::string &text() const noexcept { return text_; }

private:
  CidrV4(std::uint32_t network, std::uint32_t mask, std::string text)
      : network_(network), mask_(mask), text_(std::move(text)) {}

  std::uint32_t network_{};
  std::uint32_t mask_{};
  std::string text_;
};

struct SourceAddressConfig {
  bool spoof{};
  std::string address;
  std::vector<CidrV4> allowlist;
};

struct Config {
  Command command{Command::help};
  Profile profile{Profile::sustained};
  std::string target;
  std::uint16_t port{};
  std::uint32_t connections{1};
  std::uint64_t connection_rate{10};
  std::uint64_t pps{10};
  std::uint64_t bandwidth_bytes_per_second{125'000'000};
  std::chrono::milliseconds duration{std::chrono::seconds{10}};
  std::chrono::milliseconds timeout{std::chrono::seconds{3}};
  std::chrono::milliseconds recovery{std::chrono::seconds{3}};
  std::chrono::milliseconds interval{std::chrono::seconds{1}};
  std::size_t payload_size{512};
  std::vector<CidrV4> allowlist;
  std::string interface;
  std::vector<std::string> allowed_interfaces;
  SourceAddressConfig source_address;
  std::string json_path;
  std::string csv_path;
  SafetyLimits limits;
};

[[nodiscard]] std::uint64_t parse_bit_rate(std::string_view text);
[[nodiscard]] std::uint64_t parse_operation_rate(std::string_view text,
                                                 std::string_view suffix);
[[nodiscard]] std::chrono::milliseconds parse_duration(std::string_view text);
[[nodiscard]] double profile_factor(Profile profile,
                                    std::chrono::nanoseconds elapsed,
                                    std::chrono::nanoseconds duration) noexcept;
[[nodiscard]] std::string_view profile_name(Profile profile) noexcept;
[[nodiscard]] Config parse_command_line(int argc, char **argv);
void validate(const Config &config);
[[nodiscard]] std::string usage();

} // namespace netbench
