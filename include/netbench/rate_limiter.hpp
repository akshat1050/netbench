#pragma once

#include <chrono>
#include <cstdint>

namespace netbench {

class TokenBucket {
public:
  using Clock = std::chrono::steady_clock;

  TokenBucket(std::uint64_t bytes_per_second, std::uint64_t burst_bytes,
              Clock::time_point now = Clock::now());

  [[nodiscard]] bool try_consume(std::uint64_t bytes,
                                 Clock::time_point now = Clock::now());
  [[nodiscard]] std::chrono::nanoseconds
  delay_for(std::uint64_t bytes, Clock::time_point now = Clock::now());

private:
  void refill(Clock::time_point now);

  double rate_{};
  double capacity_{};
  double tokens_{};
  Clock::time_point last_;
};

} // namespace netbench
