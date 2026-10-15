#include "netbench/engine.hpp"

#include "netbench/socket.hpp"

#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace netbench {
namespace {

using Clock = std::chrono::steady_clock;

struct IcmpSocket {
  Socket socket;
  bool raw{};
};

std::uint16_t checksum(const void *data, std::size_t length) {
  const auto *words = static_cast<const std::uint16_t *>(data);
  std::uint32_t sum{};
  while (length > 1) {
    sum += *words++;
    length -= 2;
  }
  if (length == 1)
    sum += *reinterpret_cast<const std::uint8_t *>(words);
  while ((sum >> 16U) != 0U)
    sum = (sum & 0xffffU) + (sum >> 16U);
  return static_cast<std::uint16_t>(~sum);
}

IcmpSocket open_icmp(const Config &config) {
  int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                    IPPROTO_ICMP);
  bool raw = false;
  if (fd < 0 && (errno == EACCES || errno == EPERM)) {
    fd = ::socket(AF_INET, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC,
                  IPPROTO_ICMP);
    raw = true;
  }
  if (fd < 0)
    throw std::runtime_error(socket_error(
        "ICMP socket (check ping_group_range or CAP_NET_RAW)", errno));
  IcmpSocket result{Socket(fd), raw};
  const auto endpoint = ipv4_endpoint(config.target);
  if (::connect(fd, reinterpret_cast<const sockaddr *>(&endpoint),
                sizeof(endpoint)) < 0)
    throw std::runtime_error(socket_error("ICMP connect", errno));
  return result;
}

std::vector<std::uint8_t> echo_packet(std::size_t payload_size,
                                      std::uint16_t sequence,
                                      std::uint16_t identifier) {
  std::vector<std::uint8_t> packet(sizeof(icmphdr) + payload_size, 0xA5);
  auto *header = reinterpret_cast<icmphdr *>(packet.data());
  header->type = ICMP_ECHO;
  header->code = 0;
  header->un.echo.id = htons(identifier);
  header->un.echo.sequence = htons(sequence);
  header->checksum = 0;
  header->checksum = checksum(packet.data(), packet.size());
  return packet;
}

std::optional<std::uint16_t> reply_sequence(const std::uint8_t *data,
                                            std::size_t size, bool raw,
                                            std::uint16_t identifier) {
  std::size_t offset{};
  if (raw) {
    if (size < sizeof(iphdr))
      return std::nullopt;
    offset = static_cast<std::size_t>(data[0] & 0x0fU) * 4U;
  }
  if (size < offset + sizeof(icmphdr))
    return std::nullopt;
  const auto *header = reinterpret_cast<const icmphdr *>(data + offset);
  if (header->type != ICMP_ECHOREPLY || header->code != 0)
    return std::nullopt;
  if (raw && ntohs(header->un.echo.id) != identifier)
    return std::nullopt;
  return ntohs(header->un.echo.sequence);
}

std::optional<std::chrono::nanoseconds>
probe(int fd, bool raw, const Config &config, std::uint16_t sequence,
      std::uint16_t identifier, std::stop_token stop) {
  const auto packet = echo_packet(config.payload_size, sequence, identifier);
  const auto started = Clock::now();
  if (::send(fd, packet.data(), packet.size(), MSG_NOSIGNAL) < 0)
    return std::nullopt;
  auto epoll = make_epoll();
  epoll_add(epoll.get(), fd, EPOLLIN | EPOLLERR);
  const auto deadline = started + config.timeout;
  std::array<std::uint8_t, 2048> buffer{};
  epoll_event event{};
  while (!stop.stop_requested() && Clock::now() < deadline) {
    const int count = ::epoll_wait(epoll.get(), &event, 1, 25);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      continue;
    while (true) {
      const auto received =
          ::recv(fd, buffer.data(), buffer.size(), MSG_DONTWAIT);
      if (received < 0 && errno == EINTR)
        continue;
      if (received <= 0)
        break;
      const auto reply = reply_sequence(
          buffer.data(), static_cast<std::size_t>(received), raw, identifier);
      if (reply && *reply == sequence)
        return Clock::now() - started;
    }
  }
  return std::nullopt;
}

std::optional<LatencySummary>
one_sample(std::optional<std::chrono::nanoseconds> latency) {
  return latency ? summarize_latencies({*latency}) : std::nullopt;
}

} // namespace

TestResult run_icmp_test(const Config &config, std::stop_token stop,
                         const ProgressCallback &progress) {
  validate(config);
  auto icmp = open_icmp(config);
  const auto identifier =
      static_cast<std::uint16_t>(static_cast<unsigned>(::getpid()) & 0xffffU);
  const auto before = one_sample(
      probe(icmp.socket.get(), icmp.raw, config, 0xffffU, identifier, stop));
  const auto resource_before = read_resource_usage();
  auto epoll = make_epoll();
  epoll_add(epoll.get(), icmp.socket.get(), EPOLLIN | EPOLLERR);

  Metrics metrics;
  std::unordered_map<std::uint16_t, Clock::time_point> outstanding;
  std::array<std::uint8_t, 2048> buffer{};
  std::array<epoll_event, 8> events{};
  std::uint16_t sequence{};
  const auto started = Clock::now();
  const auto deadline = started + config.duration;
  auto next_send = started;
  auto next_report = started + config.interval;

  const auto receive_replies = [&] {
    while (true) {
      const auto received =
          ::recv(icmp.socket.get(), buffer.data(), buffer.size(), MSG_DONTWAIT);
      if (received < 0 && errno == EINTR)
        continue;
      if (received <= 0)
        break;
      const auto reply =
          reply_sequence(buffer.data(), static_cast<std::size_t>(received),
                         icmp.raw, identifier);
      if (!reply)
        continue;
      const auto found = outstanding.find(*reply);
      if (found == outstanding.end())
        continue;
      metrics.add_received(static_cast<std::uint64_t>(received));
      metrics.add_latency(Clock::now() - found->second);
      outstanding.erase(found);
    }
  };

  while (!stop.stop_requested() && Clock::now() < deadline) {
    auto now = Clock::now();
    std::size_t batch{};
    while (now >= next_send && batch++ < 256) {
      ++sequence;
      if (sequence == 0 || sequence >= 0xfffeU)
        sequence = 1;
      if (outstanding.erase(sequence) != 0)
        metrics.timeout();
      const auto packet =
          echo_packet(config.payload_size, sequence, identifier);
      metrics.attempted();
      const auto sent = ::send(icmp.socket.get(), packet.data(), packet.size(),
                               MSG_DONTWAIT | MSG_NOSIGNAL);
      if (sent >= 0) {
        metrics.add_sent(static_cast<std::uint64_t>(sent));
        outstanding.emplace(sequence, now);
      } else {
        metrics.local_send_failure();
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOBUFS)
          metrics.socket_error();
      }
      const auto factor =
          profile_factor(config.profile, now - started, config.duration);
      const auto rate = std::max(1.0, static_cast<double>(config.pps) * factor);
      next_send += std::chrono::duration_cast<Clock::duration>(
          std::chrono::duration<double>(1.0 / rate));
      now = Clock::now();
    }
    const int count = ::epoll_wait(epoll.get(), events.data(),
                                   static_cast<int>(events.size()), 10);
    if (count < 0 && errno != EINTR)
      throw std::runtime_error(socket_error("epoll_wait", errno));
    if (count > 0)
      receive_replies();
    now = Clock::now();
    for (auto iterator = outstanding.begin(); iterator != outstanding.end();) {
      if (now - iterator->second >= config.timeout) {
        metrics.timeout();
        iterator = outstanding.erase(iterator);
      } else {
        ++iterator;
      }
    }
    if (progress && now >= next_report) {
      progress(metrics.snapshot(now - started));
      next_report = now + config.interval;
    }
  }

  const auto ended = Clock::now();
  const auto resources = calculate_resource_usage(
      resource_before, read_resource_usage(), ended - started);
  const auto recovery_deadline =
      ended + std::max(config.recovery, config.timeout);
  while (!stop.stop_requested() && !outstanding.empty() &&
         Clock::now() < recovery_deadline) {
    const int count = ::epoll_wait(epoll.get(), events.data(),
                                   static_cast<int>(events.size()), 10);
    if (count > 0)
      receive_replies();
    const auto now = Clock::now();
    for (auto iterator = outstanding.begin(); iterator != outstanding.end();) {
      if (now - iterator->second >= config.timeout) {
        metrics.timeout();
        iterator = outstanding.erase(iterator);
      } else {
        ++iterator;
      }
    }
  }
  for (std::size_t index = 0; index < outstanding.size(); ++index)
    metrics.timeout();
  outstanding.clear();
  const auto after = one_sample(
      probe(icmp.socket.get(), icmp.raw, config, 0xfffeU, identifier, stop));
  auto snapshot = metrics.snapshot(ended - started);
  const auto loss = snapshot.operations_sent == 0
                        ? 0.0
                        : static_cast<double>(snapshot.operations_sent -
                                              snapshot.operations_received) *
                              100.0 /
                              static_cast<double>(snapshot.operations_sent);
  return {"icmp", snapshot, metrics.latency_summary(), before, after,
          loss,   resources};
}

} // namespace netbench
