#include "netbench/config.hpp"
#include "netbench/engine.hpp"
#include "netbench/socket.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>

#include <array>
#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <vector>

namespace {

std::uint16_t bound_port(int fd) {
  sockaddr_in endpoint{};
  socklen_t length = sizeof(endpoint);
  if (::getsockname(fd, reinterpret_cast<sockaddr *>(&endpoint), &length) < 0)
    throw std::runtime_error("getsockname failed");
  return ntohs(endpoint.sin_port);
}

netbench::Config base(netbench::Command command) {
  netbench::Config config;
  config.command = command;
  config.target = "127.0.0.1";
  config.duration = std::chrono::milliseconds{150};
  config.timeout = std::chrono::milliseconds{100};
  config.recovery = std::chrono::milliseconds{10};
  config.interval = std::chrono::seconds{1};
  return config;
}

void test_tcp() {
  netbench::Socket listener(
      ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  if (!listener)
    throw std::runtime_error("TCP listener socket failed");
  const auto endpoint = netbench::ipv4_endpoint("127.0.0.1");
  if (::bind(listener.get(), reinterpret_cast<const sockaddr *>(&endpoint),
             sizeof(endpoint)) < 0 ||
      ::listen(listener.get(), 16) < 0)
    throw std::runtime_error("TCP listener setup failed");
  std::jthread server([&](std::stop_token stop) {
    std::vector<netbench::Socket> accepted;
    while (!stop.stop_requested()) {
      const int fd = ::accept4(listener.get(), nullptr, nullptr,
                               SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (fd >= 0)
        accepted.emplace_back(fd);
      else
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
  });

  auto config = base(netbench::Command::tcp);
  config.port = bound_port(listener.get());
  config.connections = 3;
  config.connection_rate = 100;
  const auto result = netbench::run_tcp_test(config, {});
  if (result.metrics.connections_succeeded != 3 || !result.latency)
    throw std::runtime_error("TCP connection measurements failed");
}

void test_udp() {
  netbench::Socket receiver(
      ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  if (!receiver)
    throw std::runtime_error("UDP receiver socket failed");
  const auto endpoint = netbench::ipv4_endpoint("127.0.0.1");
  if (::bind(receiver.get(), reinterpret_cast<const sockaddr *>(&endpoint),
             sizeof(endpoint)) < 0)
    throw std::runtime_error("UDP bind failed");
  auto config = base(netbench::Command::udp);
  config.port = bound_port(receiver.get());
  config.pps = 100;
  config.payload_size = 64;
  config.bandwidth_bytes_per_second = 1'000'000;
  const auto result = netbench::run_udp_test(config, {});
  if (result.metrics.operations_sent == 0 ||
      result.metrics.local_send_failures != 0 || result.packet_loss_percent)
    throw std::runtime_error("UDP sender metrics failed");
  std::array<std::byte, 128> buffer{};
  if (::recv(receiver.get(), buffer.data(), buffer.size(), 0) <= 0)
    throw std::runtime_error("UDP localhost receiver saw no datagram");
}

void test_icmp() {
  auto config = base(netbench::Command::icmp);
  config.duration = std::chrono::milliseconds{200};
  config.timeout = std::chrono::milliseconds{100};
  config.pps = 20;
  config.payload_size = 16;
  const auto result = netbench::run_icmp_test(config, {});
  if (result.metrics.operations_sent == 0 ||
      result.metrics.operations_received == 0 || !result.latency ||
      !result.packet_loss_percent)
    throw std::runtime_error("ICMP localhost measurements failed");
}

} // namespace

int main() {
  try {
    test_tcp();
    test_udp();
    test_icmp();
    std::cout << "TCP, UDP, and ICMP localhost integration passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
