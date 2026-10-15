#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace netbench {

struct MetricsSnapshot {
  std::uint64_t bytes_sent{};
  std::uint64_t bytes_received{};
  std::uint64_t operations_attempted{};
  std::uint64_t operations_sent{};
  std::uint64_t operations_received{};
  std::uint64_t connections_succeeded{};
  std::uint64_t connections_failed{};
  std::uint64_t timeouts{};
  std::uint64_t resets{};
  std::uint64_t socket_errors{};
  std::uint64_t local_send_failures{};
  std::uint64_t icmp_unreachable{};
  std::optional<std::uint64_t> tcp_retransmissions;
  std::chrono::nanoseconds elapsed{};

  [[nodiscard]] double transmit_mbps() const noexcept;
  [[nodiscard]] double attempted_per_second() const noexcept;
  [[nodiscard]] double successful_per_second() const noexcept;
  [[nodiscard]] double success_percent() const noexcept;
};

struct LatencySummary {
  double minimum_ms{};
  double average_ms{};
  double maximum_ms{};
  double p50_ms{};
  double p95_ms{};
  double p99_ms{};
};

class Metrics {
public:
  void attempted() noexcept;
  void add_sent(std::uint64_t bytes) noexcept;
  void add_received(std::uint64_t bytes) noexcept;
  void connection_succeeded() noexcept;
  void connection_failed() noexcept;
  void timeout() noexcept;
  void reset() noexcept;
  void socket_error() noexcept;
  void local_send_failure() noexcept;
  void unreachable() noexcept;
  void add_latency(std::chrono::nanoseconds latency);
  [[nodiscard]] MetricsSnapshot
  snapshot(std::chrono::nanoseconds elapsed) const noexcept;
  [[nodiscard]] std::optional<LatencySummary> latency_summary() const;

private:
  std::atomic<std::uint64_t> bytes_sent_{};
  std::atomic<std::uint64_t> bytes_received_{};
  std::atomic<std::uint64_t> operations_attempted_{};
  std::atomic<std::uint64_t> operations_sent_{};
  std::atomic<std::uint64_t> operations_received_{};
  std::atomic<std::uint64_t> connections_succeeded_{};
  std::atomic<std::uint64_t> connections_failed_{};
  std::atomic<std::uint64_t> timeouts_{};
  std::atomic<std::uint64_t> resets_{};
  std::atomic<std::uint64_t> socket_errors_{};
  std::atomic<std::uint64_t> local_send_failures_{};
  std::atomic<std::uint64_t> icmp_unreachable_{};
  mutable std::mutex latency_mutex_;
  std::vector<std::chrono::nanoseconds> latencies_;
};

struct ResourceUsage {
  double cpu_percent{};
  std::uint64_t peak_memory_kib{};
};

struct ResourceReading {
  double cpu_seconds{};
  std::uint64_t peak_memory_kib{};
};

[[nodiscard]] ResourceReading read_resource_usage() noexcept;
[[nodiscard]] ResourceUsage
calculate_resource_usage(ResourceReading before, ResourceReading after,
                         std::chrono::nanoseconds elapsed) noexcept;
[[nodiscard]] std::optional<LatencySummary>
summarize_latencies(std::vector<std::chrono::nanoseconds> samples);

} // namespace netbench
