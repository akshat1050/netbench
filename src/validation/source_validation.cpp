#include "netbench/source_validation.hpp"

#include "netbench/engine.hpp"
#include "netbench/socket.hpp"
#include "netbench/source_policy.hpp"

#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace netbench {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::uint16_t probe_identifier = 0x4e42;
constexpr std::uint32_t probe_magic = 0x4e425356;

struct ProbePacket {
  iphdr ip{};
  icmphdr icmp{};
  std::uint32_t magic{};
  std::uint32_t sequence{};
};

static_assert(sizeof(ProbePacket) == 36);

std::uint16_t checksum(const void *data, std::size_t size) {
  const auto *bytes = static_cast<const std::byte *>(data);
  std::uint32_t sum{};
  while (size >= 2) {
    std::uint16_t word{};
    std::memcpy(&word, bytes, sizeof(word));
    sum += word;
    bytes += 2;
    size -= 2;
  }
  if (size != 0)
    sum += std::to_integer<std::uint8_t>(*bytes);
  while ((sum >> 16U) != 0)
    sum = (sum & 0xffffU) + (sum >> 16U);
  return static_cast<std::uint16_t>(~sum);
}

ProbePacket make_probe(in_addr source, in_addr destination,
                       std::uint16_t sequence) {
  ProbePacket packet{};
  packet.ip.version = 4;
  packet.ip.ihl = 5;
  packet.ip.tot_len = htons(static_cast<std::uint16_t>(sizeof(packet)));
  packet.ip.id = htons(static_cast<std::uint16_t>(0x4e00U + sequence));
  packet.ip.frag_off = htons(IP_DF);
  packet.ip.ttl = 64;
  packet.ip.protocol = IPPROTO_ICMP;
  packet.ip.saddr = source.s_addr;
  packet.ip.daddr = destination.s_addr;
  packet.ip.check = checksum(&packet.ip, sizeof(packet.ip));

  packet.icmp.type = ICMP_ECHO;
  packet.icmp.code = 0;
  packet.icmp.un.echo.id = htons(probe_identifier);
  packet.icmp.un.echo.sequence = htons(sequence);
  packet.magic = htonl(probe_magic);
  packet.sequence = htonl(sequence);
  packet.icmp.checksum =
      checksum(&packet.icmp, sizeof(packet) - sizeof(packet.ip));
  return packet;
}

bool matching_embedded_probe(const std::byte *data, std::size_t size,
                             in_addr source, in_addr destination,
                             std::uint16_t &sequence) {
  if (size < sizeof(iphdr))
    return false;
  iphdr ip{};
  std::memcpy(&ip, data, sizeof(ip));
  const auto ip_size = static_cast<std::size_t>(ip.ihl) * 4U;
  if (ip.version != 4 || ip_size < sizeof(iphdr) ||
      size < ip_size + sizeof(icmphdr) || ip.protocol != IPPROTO_ICMP ||
      ip.saddr != source.s_addr || ip.daddr != destination.s_addr)
    return false;
  icmphdr icmp{};
  std::memcpy(&icmp, data + static_cast<std::ptrdiff_t>(ip_size), sizeof(icmp));
  if (icmp.type != ICMP_ECHO || ntohs(icmp.un.echo.id) != probe_identifier)
    return false;
  sequence = ntohs(icmp.un.echo.sequence);
  return true;
}

void observe_responses(int fd, in_addr source, in_addr destination,
                       const std::vector<Clock::time_point> &sent_at,
                       std::vector<bool> &observed, Metrics &metrics,
                       std::vector<std::string> &observations,
                       std::vector<std::string> &local_errors) {
  std::array<std::byte, 2048> buffer{};
  while (true) {
    const auto received = ::recv(fd, buffer.data(), buffer.size(), 0);
    if (received < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        metrics.socket_error();
        local_errors.push_back(socket_error("receive ICMP observation", errno));
      }
      return;
    }
    const auto size = static_cast<std::size_t>(received);
    if (size < sizeof(iphdr))
      continue;
    iphdr ip{};
    std::memcpy(&ip, buffer.data(), sizeof(ip));
    const auto ip_size = static_cast<std::size_t>(ip.ihl) * 4U;
    if (ip.version != 4 || ip_size < sizeof(iphdr) ||
        size < ip_size + sizeof(icmphdr) || ip.protocol != IPPROTO_ICMP)
      continue;
    icmphdr icmp{};
    std::memcpy(&icmp, buffer.data() + static_cast<std::ptrdiff_t>(ip_size),
                sizeof(icmp));
    std::uint16_t sequence{};
    std::string observation;
    if (icmp.type == ICMP_ECHOREPLY && ip.saddr == destination.s_addr &&
        ip.daddr == source.s_addr &&
        ntohs(icmp.un.echo.id) == probe_identifier) {
      sequence = ntohs(icmp.un.echo.sequence);
      if (size < ip_size + sizeof(icmphdr) + 2 * sizeof(std::uint32_t))
        continue;
      std::uint32_t magic{};
      std::uint32_t payload_sequence{};
      const auto *payload = buffer.data() +
                            static_cast<std::ptrdiff_t>(ip_size) +
                            sizeof(icmphdr);
      std::memcpy(&magic, payload, sizeof(magic));
      std::memcpy(&payload_sequence,
                  payload + static_cast<std::ptrdiff_t>(sizeof(magic)),
                  sizeof(payload_sequence));
      if (ntohl(magic) != probe_magic || ntohl(payload_sequence) != sequence)
        continue;
      observation = "ICMP echo reply for probe " + std::to_string(sequence);
    } else if ((icmp.type == ICMP_DEST_UNREACH ||
                icmp.type == ICMP_TIME_EXCEEDED) &&
               matching_embedded_probe(
                   buffer.data() + static_cast<std::ptrdiff_t>(ip_size) +
                       sizeof(icmphdr),
                   size - ip_size - sizeof(icmphdr), source, destination,
                   sequence)) {
      observation =
          icmp.type == ICMP_DEST_UNREACH
              ? "ICMP unreachable for probe " + std::to_string(sequence)
              : "ICMP time exceeded for probe " + std::to_string(sequence);
      if (icmp.type == ICMP_DEST_UNREACH)
        metrics.unreachable();
    } else {
      continue;
    }
    if (sequence == 0 || sequence >= observed.size() || observed[sequence])
      continue;
    observed[sequence] = true;
    metrics.add_received(static_cast<std::uint64_t>(size));
    if (sequence < sent_at.size() && sent_at[sequence] != Clock::time_point{})
      metrics.add_latency(Clock::now() - sent_at[sequence]);
    observations.push_back(std::move(observation));
  }
}

} // namespace

TestResult SourceValidationEngine::run(
    const Config &config, std::stop_token stop,
    const std::function<void(const MetricsSnapshot &)> &progress) {
  validate(config);
  const auto environment = LabEnvironment::verify(config);

  in_addr source{};
  in_addr destination{};
  if (::inet_pton(AF_INET, config.source_address.address.c_str(), &source) !=
          1 ||
      ::inet_pton(AF_INET, config.target.c_str(), &destination) != 1)
    throw std::invalid_argument("source validation requires IPv4 literals");

  Socket socket(
      ::socket(AF_INET, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_ICMP));
  if (!socket)
    throw std::runtime_error(socket_error("create raw ICMP socket", errno));
  const int enabled = 1;
  if (::setsockopt(socket.get(), IPPROTO_IP, IP_HDRINCL, &enabled,
                   sizeof(enabled)) < 0)
    throw std::runtime_error(socket_error("enable IP_HDRINCL", errno));
  if (::setsockopt(socket.get(), SOL_SOCKET, SO_BINDTODEVICE,
                   config.interface.c_str(),
                   static_cast<socklen_t>(config.interface.size() + 1)) < 0)
    throw std::runtime_error(
        socket_error("bind raw socket to lab interface", errno));

  const auto resource_before = read_resource_usage();
  const auto started = Clock::now();
  const auto deadline = started + config.duration;
  const auto period = std::chrono::nanoseconds{
      1'000'000'000LL / static_cast<std::int64_t>(config.pps)};
  const auto probe_limit = std::min<std::uint64_t>(
      SourcePolicy::maximum_probes,
      (static_cast<std::uint64_t>(config.duration.count()) * config.pps + 999) /
          1000);
  std::vector<Clock::time_point> sent_at(probe_limit + 1);
  std::vector<bool> observed(probe_limit + 1);
  std::vector<std::string> observations;
  std::vector<std::string> local_errors;
  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_addr = destination;
  auto next_send = started;
  auto next_report = started + config.interval;
  std::uint16_t sequence = 1;
  Metrics metrics;

  while (!stop.stop_requested() && Clock::now() < deadline) {
    const auto now = Clock::now();
    observe_responses(socket.get(), source, destination, sent_at, observed,
                      metrics, observations, local_errors);
    if (sequence <= probe_limit && now >= next_send) {
      SourcePolicy::validate(config);
      const auto current_environment = LabEnvironment::verify(config);
      if (current_environment.actual_interface !=
              environment.actual_interface ||
          current_environment.test_namespace != environment.test_namespace)
        throw std::runtime_error("lab environment changed during validation");

      const auto packet = make_probe(source, destination, sequence);
      metrics.attempted();
      const auto sent =
          ::sendto(socket.get(), &packet, sizeof(packet), 0,
                   reinterpret_cast<const sockaddr *>(&target), sizeof(target));
      if (sent == static_cast<ssize_t>(sizeof(packet))) {
        sent_at[sequence] = Clock::now();
        metrics.add_sent(sizeof(packet));
      } else {
        metrics.local_send_failure();
        local_errors.push_back(
            sent < 0 ? socket_error("transmit validation probe", errno)
                     : "validation probe was only partially transmitted");
      }
      ++sequence;
      next_send += period;
    }
    if (progress && now >= next_report) {
      progress(metrics.snapshot(now - started));
      next_report = now + config.interval;
    }

    const auto wake = std::min(
        {deadline, next_send, Clock::now() + std::chrono::milliseconds{20}});
    const auto wait =
        std::max(std::chrono::milliseconds{1},
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     wake - Clock::now()));
    pollfd descriptor{socket.get(), POLLIN, 0};
    if (::poll(&descriptor, 1, static_cast<int>(wait.count())) < 0 &&
        errno != EINTR) {
      metrics.socket_error();
      local_errors.push_back(socket_error("poll validation socket", errno));
      break;
    }
  }

  observe_responses(socket.get(), source, destination, sent_at, observed,
                    metrics, observations, local_errors);
  const auto ended = Clock::now();
  const auto snapshot = metrics.snapshot(ended - started);
  const auto resources = calculate_resource_usage(
      resource_before, read_resource_usage(), ended - started);
  TestResult result{"source-validation",
                    snapshot,
                    metrics.latency_summary(),
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    resources};
  result.source_validation =
      SourceValidationReport{environment.actual_interface,
                             config.source_address.address,
                             config.target,
                             environment.test_namespace,
                             snapshot.operations_sent,
                             std::move(local_errors),
                             std::move(observations),
                             false};
  return result;
}

} // namespace netbench
