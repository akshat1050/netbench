#include "netbench/metrics.hpp"

#include <sys/resource.h>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace netbench {
namespace {

double per_second(std::uint64_t value,
                  std::chrono::nanoseconds elapsed) noexcept {
  if (elapsed <= std::chrono::nanoseconds::zero())
    return 0.0;
  return static_cast<double>(value) /
         std::chrono::duration<double>(elapsed).count();
}

double percentile(const std::vector<std::chrono::nanoseconds> &samples,
                  double fraction) {
  const auto rank = static_cast<std::size_t>(
      std::ceil(fraction * static_cast<double>(samples.size())));
  const auto index = std::min(samples.size() - 1, rank == 0 ? 0 : rank - 1);
  return std::chrono::duration<double, std::milli>(samples[index]).count();
}

double timeval_seconds(timeval value) noexcept {
  return static_cast<double>(value.tv_sec) +
         static_cast<double>(value.tv_usec) / 1'000'000.0;
}

} // namespace

double MetricsSnapshot::transmit_mbps() const noexcept {
  return per_second(bytes_sent, elapsed) * 8.0 / 1'000'000.0;
}

double MetricsSnapshot::attempted_per_second() const noexcept {
  return per_second(operations_attempted, elapsed);
}

double MetricsSnapshot::successful_per_second() const noexcept {
  return per_second(operations_sent, elapsed);
}

double MetricsSnapshot::success_percent() const noexcept {
  if (operations_attempted == 0)
    return 0.0;
  return static_cast<double>(operations_sent) * 100.0 /
         static_cast<double>(operations_attempted);
}

void Metrics::attempted() noexcept {
  operations_attempted_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::add_sent(std::uint64_t bytes) noexcept {
  bytes_sent_.fetch_add(bytes, std::memory_order_relaxed);
  operations_sent_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::add_received(std::uint64_t bytes) noexcept {
  bytes_received_.fetch_add(bytes, std::memory_order_relaxed);
  operations_received_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::connection_succeeded() noexcept {
  connections_succeeded_.fetch_add(1, std::memory_order_relaxed);
  operations_sent_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::connection_failed() noexcept {
  connections_failed_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::timeout() noexcept {
  timeouts_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::reset() noexcept {
  resets_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::socket_error() noexcept {
  socket_errors_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::local_send_failure() noexcept {
  local_send_failures_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::unreachable() noexcept {
  icmp_unreachable_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::add_latency(std::chrono::nanoseconds latency) {
  std::lock_guard lock(latency_mutex_);
  latencies_.push_back(latency);
}

MetricsSnapshot
Metrics::snapshot(std::chrono::nanoseconds elapsed) const noexcept {
  return {bytes_sent_.load(std::memory_order_relaxed),
          bytes_received_.load(std::memory_order_relaxed),
          operations_attempted_.load(std::memory_order_relaxed),
          operations_sent_.load(std::memory_order_relaxed),
          operations_received_.load(std::memory_order_relaxed),
          connections_succeeded_.load(std::memory_order_relaxed),
          connections_failed_.load(std::memory_order_relaxed),
          timeouts_.load(std::memory_order_relaxed),
          resets_.load(std::memory_order_relaxed),
          socket_errors_.load(std::memory_order_relaxed),
          local_send_failures_.load(std::memory_order_relaxed),
          icmp_unreachable_.load(std::memory_order_relaxed),
          std::nullopt,
          elapsed};
}

std::optional<LatencySummary> Metrics::latency_summary() const {
  std::lock_guard lock(latency_mutex_);
  return summarize_latencies(latencies_);
}

ResourceReading read_resource_usage() noexcept {
  rusage usage{};
  if (::getrusage(RUSAGE_SELF, &usage) != 0)
    return {};
  return {timeval_seconds(usage.ru_utime) + timeval_seconds(usage.ru_stime),
          static_cast<std::uint64_t>(std::max(usage.ru_maxrss, 0L))};
}

ResourceUsage
calculate_resource_usage(ResourceReading before, ResourceReading after,
                         std::chrono::nanoseconds elapsed) noexcept {
  const auto seconds = std::chrono::duration<double>(elapsed).count();
  return {seconds <= 0.0
              ? 0.0
              : (after.cpu_seconds - before.cpu_seconds) * 100.0 / seconds,
          after.peak_memory_kib};
}

std::optional<LatencySummary>
summarize_latencies(std::vector<std::chrono::nanoseconds> samples) {
  if (samples.empty())
    return std::nullopt;
  std::ranges::sort(samples);
  const auto total = std::accumulate(samples.begin(), samples.end(),
                                     std::chrono::nanoseconds{});
  return LatencySummary{
      std::chrono::duration<double, std::milli>(samples.front()).count(),
      std::chrono::duration<double, std::milli>(total).count() /
          static_cast<double>(samples.size()),
      std::chrono::duration<double, std::milli>(samples.back()).count(),
      percentile(samples, 0.50),
      percentile(samples, 0.95),
      percentile(samples, 0.99)};
}

} // namespace netbench
