#include "netbench/engine.hpp"

#include "netbench/rate_limiter.hpp"
#include "netbench/socket.hpp"

#include <linux/errqueue.h>
#include <netinet/ip_icmp.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace netbench {
namespace {

using Clock = std::chrono::steady_clock;

void drain_error_queue(int fd, Metrics &metrics) {
  std::array<std::byte, 1> payload{};
  std::array<std::byte, 512> control{};
  iovec io{payload.data(), payload.size()};
  while (true) {
    msghdr message{};
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    if (::recvmsg(fd, &message, MSG_ERRQUEUE | MSG_DONTWAIT) < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return;
      if (errno == EINTR)
        continue;
      metrics.socket_error();
      return;
    }
    for (auto *header = CMSG_FIRSTHDR(&message); header != nullptr;
         header = CMSG_NXTHDR(&message, header)) {
      if (header->cmsg_level != IPPROTO_IP || header->cmsg_type != IP_RECVERR)
        continue;
      const auto *error =
          reinterpret_cast<const sock_extended_err *>(CMSG_DATA(header));
      if (error->ee_origin == SO_EE_ORIGIN_ICMP &&
          error->ee_type == ICMP_DEST_UNREACH)
        metrics.unreachable();
    }
  }
}

void drain_socket_error(int fd, Metrics &metrics) {
  int error{};
  socklen_t length = sizeof(error);
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0) {
    metrics.socket_error();
  } else if (error == ECONNREFUSED) {
    metrics.unreachable();
  } else if (error != 0) {
    metrics.socket_error();
  }
}

} // namespace

TestResult run_udp_test(const Config &config, std::stop_token stop,
                        const ProgressCallback &progress) {
  validate(config);
  const auto resource_before = read_resource_usage();
  const auto wall_started = Clock::now();
  Socket socket(
      ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  if (!socket)
    throw std::runtime_error(socket_error("UDP socket", errno));
  const int enabled = 1;
  const bool extended_errors =
      ::setsockopt(socket.get(), IPPROTO_IP, IP_RECVERR, &enabled,
                   sizeof(enabled)) == 0;
  const auto endpoint = ipv4_endpoint(config.target, config.port);
  if (::connect(socket.get(), reinterpret_cast<const sockaddr *>(&endpoint),
                sizeof(endpoint)) < 0)
    throw std::runtime_error(socket_error("UDP connect", errno));
  auto epoll = make_epoll();
  epoll_add(epoll.get(), socket.get(), EPOLLOUT | EPOLLERR);

  Metrics metrics;
  std::vector<std::uint8_t> payload(config.payload_size, 0xA5);
  TokenBucket bandwidth(
      config.bandwidth_bytes_per_second,
      std::max<std::uint64_t>(config.payload_size,
                              config.bandwidth_bytes_per_second / 100U));
  const auto started = Clock::now();
  const auto deadline = started + config.duration;
  auto next_send = started;
  auto next_report = started + config.interval;
  epoll_event event{};

  while (!stop.stop_requested() && Clock::now() < deadline) {
    auto now = Clock::now();
    std::size_t batch{};
    while (now >= next_send && batch++ < 256) {
      const auto factor =
          profile_factor(config.profile, now - started, config.duration);
      const auto rate = std::max(1.0, static_cast<double>(config.pps) * factor);
      if (!bandwidth.try_consume(payload.size(), now))
        break;
      metrics.attempted();
      const auto sent = ::send(socket.get(), payload.data(), payload.size(),
                               MSG_DONTWAIT | MSG_NOSIGNAL);
      if (sent >= 0) {
        metrics.add_sent(static_cast<std::uint64_t>(sent));
      } else {
        metrics.local_send_failure();
        if (errno == ECONNREFUSED)
          metrics.unreachable();
        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOBUFS)
          metrics.socket_error();
      }
      next_send += std::chrono::duration_cast<Clock::duration>(
          std::chrono::duration<double>(1.0 / rate));
      now = Clock::now();
    }

    const int wait_ms =
        now < next_send
            ? std::clamp(
                  static_cast<int>(
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                          next_send - now)
                          .count() +
                      1),
                  1, 10)
            : 0;
    const int count = ::epoll_wait(epoll.get(), &event, 1, wait_ms);
    if (count < 0 && errno != EINTR)
      throw std::runtime_error(socket_error("epoll_wait", errno));
    if (count > 0 && (event.events & EPOLLERR) != 0U) {
      if (extended_errors)
        drain_error_queue(socket.get(), metrics);
      else
        drain_socket_error(socket.get(), metrics);
    }
    now = Clock::now();
    if (progress && now >= next_report) {
      progress(metrics.snapshot(now - started));
      next_report = now + config.interval;
    }
  }
  const auto ended = Clock::now();
  const auto resources = calculate_resource_usage(
      resource_before, read_resource_usage(), ended - wall_started);
  const auto recovery_deadline = ended + config.recovery;
  while (!stop.stop_requested() && Clock::now() < recovery_deadline) {
    const int count = ::epoll_wait(epoll.get(), &event, 1, 10);
    if (count > 0 && (event.events & EPOLLERR) != 0U) {
      if (extended_errors)
        drain_error_queue(socket.get(), metrics);
      else
        drain_socket_error(socket.get(), metrics);
    }
  }
  if (extended_errors)
    drain_error_queue(socket.get(), metrics);
  return {"udp",        metrics.snapshot(ended - started),
          std::nullopt, std::nullopt,
          std::nullopt, std::nullopt,
          resources};
}

} // namespace netbench
