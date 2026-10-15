#include "netbench/engine.hpp"

#include "netbench/socket.hpp"

#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace netbench {
namespace {

using Clock = std::chrono::steady_clock;

enum class State { connecting, established };

struct Connection {
  Socket socket;
  State state{State::connecting};
  Clock::time_point started;
};

std::optional<std::chrono::nanoseconds> connect_probe(const Config &config,
                                                      std::stop_token stop) {
  Socket socket(
      ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  if (!socket)
    return std::nullopt;
  const auto endpoint = ipv4_endpoint(config.target, config.port);
  const auto started = Clock::now();
  if (::connect(socket.get(), reinterpret_cast<const sockaddr *>(&endpoint),
                sizeof(endpoint)) == 0)
    return Clock::now() - started;
  if (errno != EINPROGRESS)
    return std::nullopt;
  auto epoll = make_epoll();
  epoll_add(epoll.get(), socket.get(), EPOLLOUT | EPOLLERR);
  epoll_event event{};
  const auto deadline = started + config.timeout;
  while (!stop.stop_requested() && Clock::now() < deadline) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                              Clock::now());
    const int count = ::epoll_wait(
        epoll.get(), &event, 1,
        std::clamp(static_cast<int>(remaining.count() + 1), 1, 50));
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      continue;
    int error{};
    socklen_t length = sizeof(error);
    if (::getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, &error, &length) ==
            0 &&
        error == 0)
      return Clock::now() - started;
    return std::nullopt;
  }
  return std::nullopt;
}

void wait_recovery(std::stop_token stop, std::chrono::milliseconds duration) {
  const auto deadline = Clock::now() + duration;
  while (!stop.stop_requested() && Clock::now() < deadline)
    std::this_thread::sleep_for(
        std::min(std::chrono::milliseconds{25},
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     deadline - Clock::now())));
}

std::optional<LatencySummary>
one_sample(std::optional<std::chrono::nanoseconds> latency) {
  if (!latency)
    return std::nullopt;
  return summarize_latencies({*latency});
}

} // namespace

TestResult run_tcp_test(const Config &config, std::stop_token stop,
                        const ProgressCallback &progress) {
  validate(config);
  const auto before = one_sample(connect_probe(config, stop));
  const auto resource_before = read_resource_usage();
  const auto endpoint = ipv4_endpoint(config.target, config.port);
  auto epoll = make_epoll();
  Metrics metrics;
  std::unordered_map<int, Connection> sockets;
  std::array<epoll_event, 256> events{};
  const auto started = Clock::now();
  const auto deadline = started + config.duration;
  auto next_launch = started;
  auto next_report = started + config.interval;
  std::uint32_t launched{};

  while (!stop.stop_requested() && Clock::now() < deadline) {
    auto now = Clock::now();
    while (launched < config.connections && now >= next_launch) {
      metrics.attempted();
      ++launched;
      Socket socket(
          ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
      if (!socket) {
        metrics.connection_failed();
        metrics.socket_error();
      } else {
        Connection connection;
        connection.started = now;
        const int result = ::connect(
            socket.get(), reinterpret_cast<const sockaddr *>(&endpoint),
            sizeof(endpoint));
        if (result == 0) {
          connection.state = State::established;
          metrics.connection_succeeded();
          metrics.add_latency(Clock::now() - connection.started);
        } else if (errno != EINPROGRESS) {
          metrics.connection_failed();
          if (errno == ECONNRESET)
            metrics.reset();
          else
            metrics.socket_error();
          socket.reset();
        }
        if (socket) {
          const int fd = socket.get();
          connection.socket = std::move(socket);
          epoll_add(epoll.get(), fd,
                    EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLERR | EPOLLHUP);
          sockets.emplace(fd, std::move(connection));
        }
      }
      const auto factor =
          profile_factor(config.profile, now - started, config.duration);
      const auto rate =
          std::max(1.0, static_cast<double>(config.connection_rate) * factor);
      next_launch += std::chrono::duration_cast<Clock::duration>(
          std::chrono::duration<double>(1.0 / rate));
      now = Clock::now();
    }

    const int count = ::epoll_wait(epoll.get(), events.data(),
                                   static_cast<int>(events.size()), 10);
    if (count < 0 && errno != EINTR)
      throw std::runtime_error(socket_error("epoll_wait", errno));
    for (int index = 0; index < std::max(count, 0); ++index) {
      const int fd = events[static_cast<std::size_t>(index)].data.fd;
      const auto flags = events[static_cast<std::size_t>(index)].events;
      const auto found = sockets.find(fd);
      if (found == sockets.end())
        continue;
      auto &connection = found->second;
      if (connection.state == State::connecting &&
          (flags & (EPOLLOUT | EPOLLERR | EPOLLHUP)) != 0U) {
        int error{};
        socklen_t length = sizeof(error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 &&
            error == 0) {
          connection.state = State::established;
          metrics.connection_succeeded();
          metrics.add_latency(Clock::now() - connection.started);
          epoll_modify(epoll.get(), fd,
                       EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP);
        } else {
          metrics.connection_failed();
          if (error == ECONNRESET)
            metrics.reset();
          else
            metrics.socket_error();
          sockets.erase(found);
          continue;
        }
      }
      if (connection.state == State::established &&
          (flags & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0U) {
        int error{};
        socklen_t length = sizeof(error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 &&
            error == ECONNRESET)
          metrics.reset();
        else if (error != 0)
          metrics.socket_error();
        sockets.erase(found);
      }
    }

    now = Clock::now();
    for (auto iterator = sockets.begin(); iterator != sockets.end();) {
      if (iterator->second.state == State::connecting &&
          now - iterator->second.started >= config.timeout) {
        metrics.connection_failed();
        metrics.timeout();
        iterator = sockets.erase(iterator);
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
  for (auto iterator = sockets.begin(); iterator != sockets.end();) {
    if (iterator->second.state == State::connecting) {
      metrics.connection_failed();
      metrics.timeout();
      iterator = sockets.erase(iterator);
    } else {
      ++iterator;
    }
  }
  std::uint64_t retransmissions{};
  bool retransmissions_available = false;
  for (const auto &[fd, connection] : sockets) {
    if (connection.state != State::established)
      continue;
    tcp_info info{};
    socklen_t length = sizeof(info);
    if (::getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &length) == 0) {
      retransmissions += info.tcpi_total_retrans;
      retransmissions_available = true;
    }
  }
  auto snapshot = metrics.snapshot(ended - started);
  if (retransmissions_available)
    snapshot.tcp_retransmissions = retransmissions;
  const auto resources = calculate_resource_usage(
      resource_before, read_resource_usage(), ended - started);
  sockets.clear();
  wait_recovery(stop, config.recovery);
  const auto after = one_sample(connect_probe(config, stop));
  return {"tcp",        snapshot, metrics.latency_summary(), before, after,
          std::nullopt, resources};
}

} // namespace netbench
