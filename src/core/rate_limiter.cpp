#include "netbench/rate_limiter.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace netbench {

TokenBucket::TokenBucket(std::uint64_t bytes_per_second,
                         std::uint64_t burst_bytes, Clock::time_point now)
    : rate_(static_cast<double>(bytes_per_second)),
      capacity_(static_cast<double>(burst_bytes)),
      tokens_(static_cast<double>(burst_bytes)), last_(now) {
  if (bytes_per_second == 0 || burst_bytes == 0)
    throw std::invalid_argument("token bucket rate and burst must be positive");
}

void TokenBucket::refill(Clock::time_point now) {
  if (now <= last_)
    return;
  tokens_ = std::min(
      capacity_,
      tokens_ + std::chrono::duration<double>(now - last_).count() * rate_);
  last_ = now;
}

bool TokenBucket::try_consume(std::uint64_t bytes, Clock::time_point now) {
  refill(now);
  const auto requested = static_cast<double>(bytes);
  if (requested > capacity_ || requested > tokens_)
    return false;
  tokens_ -= requested;
  return true;
}

std::chrono::nanoseconds TokenBucket::delay_for(std::uint64_t bytes,
                                                Clock::time_point now) {
  refill(now);
  const auto requested = static_cast<double>(bytes);
  if (requested <= tokens_)
    return std::chrono::nanoseconds::zero();
  const auto seconds = (requested - tokens_) / rate_;
  return std::chrono::nanoseconds{
      static_cast<std::int64_t>(std::ceil(seconds * 1'000'000'000.0))};
}

} // namespace netbench
