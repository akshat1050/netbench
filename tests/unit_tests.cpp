#include "netbench/config.hpp"
#include "netbench/metrics.hpp"
#include "netbench/rate_limiter.hpp"
#include "netbench/report.hpp"

#include <chrono>
#include <cmath>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool condition, const char *expression, int line) {
  if (!condition)
    throw std::runtime_error(std::string("check failed at line ") +
                             std::to_string(line) + ": " + expression);
}

#define CHECK(expression) check((expression), #expression, __LINE__)

template <typename Function> void check_throws(Function &&function) {
  try {
    function();
  } catch (const std::exception &) {
    return;
  }
  throw std::runtime_error("expected exception was not thrown");
}

netbench::Config base(netbench::Command command) {
  netbench::Config config;
  config.command = command;
  config.target = "127.0.0.1";
  config.allowlist.push_back(netbench::CidrV4::parse("127.0.0.0/8"));
  return config;
}

netbench::Config parse(std::vector<std::string> arguments) {
  std::vector<char *> values;
  values.reserve(arguments.size());
  for (auto &argument : arguments)
    values.push_back(argument.data());
  return netbench::parse_command_line(static_cast<int>(values.size()),
                                      values.data());
}

void test_parsing_and_authorization() {
  CHECK(netbench::parse_bit_rate("100mbps") == 12'500'000);
  CHECK(netbench::parse_operation_rate("50cps", "cps") == 50);
  CHECK(netbench::parse_operation_rate("1000", "pps") == 1000);
  CHECK(netbench::parse_duration("150ms") == std::chrono::milliseconds{150});
  check_throws([] { (void)netbench::parse_bit_rate("10"); });
  check_throws([] { (void)netbench::parse_duration("0s"); });

  const auto subnet = netbench::CidrV4::parse("192.0.2.0/24");
  CHECK(subnet.contains("192.0.2.20"));
  CHECK(!subnet.contains("192.0.3.1"));

  auto tcp = base(netbench::Command::tcp);
  tcp.port = 443;
  netbench::validate(tcp);
  tcp.connections = tcp.limits.concurrency + 1;
  check_throws([&] { netbench::validate(tcp); });

  auto udp = base(netbench::Command::udp);
  udp.port = 9000;
  udp.pps = 1000;
  udp.payload_size = 512;
  udp.bandwidth_bytes_per_second = 1;
  check_throws([&] { netbench::validate(udp); });

  auto burst = base(netbench::Command::icmp);
  burst.profile = netbench::Profile::burst;
  burst.duration = std::chrono::seconds{11};
  check_throws([&] { netbench::validate(burst); });
  CHECK(std::abs(netbench::profile_factor(netbench::Profile::ramp,
                                          std::chrono::seconds{5},
                                          std::chrono::seconds{10}) -
                 0.55) < 0.0001);

  const auto tcp_cli =
      parse({"netbench", "tcp", "--target", "192.0.2.10", "--allow-cidr",
             "192.0.2.0/24", "--port", "443", "--connections", "100", "--rate",
             "50cps", "--duration", "30s"});
  CHECK(tcp_cli.command == netbench::Command::tcp);
  CHECK(tcp_cli.connections == 100);
  CHECK(tcp_cli.connection_rate == 50);

  const auto udp_cli =
      parse({"netbench", "udp", "--target", "192.0.2.10", "--allow-cidr",
             "192.0.2.0/24", "--port", "9000", "--pps", "1000", "--size", "512",
             "--duration", "30s"});
  CHECK(udp_cli.command == netbench::Command::udp);
  CHECK(udp_cli.pps == 1000);

  const auto icmp_cli =
      parse({"netbench", "icmp", "--target", "192.0.2.10", "--allow-cidr",
             "192.0.2.0/24", "--rate", "10pps", "--duration", "30s"});
  CHECK(icmp_cli.command == netbench::Command::icmp);
  CHECK(icmp_cli.payload_size == 56);
  check_throws([] { (void)parse({"netbench", "receive"}); });
}

void test_token_bucket() {
  using namespace std::chrono_literals;
  const auto start = netbench::TokenBucket::Clock::time_point{};
  netbench::TokenBucket bucket(1000, 100, start);
  CHECK(bucket.try_consume(100, start));
  CHECK(!bucket.try_consume(1, start));
  CHECK(bucket.delay_for(50, start) == 50ms);
  CHECK(bucket.try_consume(50, start + 50ms));
}

void test_metrics_and_exports() {
  netbench::Metrics metrics;
  metrics.attempted();
  metrics.add_sent(1'000'000);
  metrics.add_received(64);
  metrics.add_latency(std::chrono::milliseconds{2});
  const auto snapshot = metrics.snapshot(std::chrono::seconds{2});
  CHECK(std::abs(snapshot.transmit_mbps() - 4.0) < 0.0001);
  CHECK(snapshot.operations_attempted == 1);
  CHECK(metrics.latency_summary()->p50_ms == 2.0);

  auto config = base(netbench::Command::icmp);
  netbench::TestResult result{
      "icmp", snapshot, metrics.latency_summary(), std::nullopt, std::nullopt,
      0.0,    {1.0, 42}};
  std::ostringstream json;
  std::ostringstream csv;
  netbench::write_json(json, config, result);
  netbench::write_csv(csv, config, result);
  CHECK(json.str().find("\"protocol\":\"icmp\"") != std::string::npos);
  CHECK(csv.str().find("version,protocol") == 0);
}

} // namespace

int main() {
  try {
    test_parsing_and_authorization();
    test_token_bucket();
    test_metrics_and_exports();
    std::cout << "unit tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
